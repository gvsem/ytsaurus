package spyt

import sbt.Keys._
import sbt.PluginTrigger.NoTrigger
import sbt._
import sbtrelease.ReleasePlugin.autoImport._
import sbtrelease.ReleaseStateTransformations._
import sbtrelease._
import spyt.SparkPackagePlugin.autoImport._
import spyt.SpytRelease._

import scala.sys.process.{Process, stderr}
import java.io.File
import java.net.http.{HttpClient, HttpRequest}
import java.net.http.HttpResponse.BodyHandlers
import java.time.Duration

object SpytPlugin extends AutoPlugin {
  override def trigger = NoTrigger

  override def requires = super.requires

  object autoImport {
    val spytClusterVersion = settingKey[String]("Spyt cluster version")
    val spytClientVersion = settingKey[String]("Spyt client version")
    val spytClientPythonVersion = settingKey[String]("yandex-spyt version")
    val spytSparkPythonVersion = settingKey[String]("yandex-spark version")

    val pypiRegistry = settingKey[String]("PyPi registry to use")

    val spytUpdateClientPythonDevVersion = taskKey[Unit]("Update yandex-spyt version config")
    val spytUpdateSparkPythonDevVersion = taskKey[Unit]("Update yandex-pyspark version config")
    val spytUpdateAllPythonDevVersions = taskKey[Unit]("Update all dev versions if needed")
    val spytUpdateClientSnapshotVersion = taskKey[Unit]("Spyt calculate real client version")

    val spytPublishClusterSnapshot = taskKey[Unit]("Publish spyt cluster with snapshot version")
    val spytPublishClientSnapshot = taskKey[Unit]("Publish spyt client with snapshot version")
    val spytPublishAllSnapshots = taskKey[Unit]("Publish spyt client & cluster with snapshot version")

    val spytPublishCluster = taskKey[Unit]("Publish spyt cluster")
    val spytPublishClient = taskKey[Unit]("Publish spyt client")
    val spytPublishAll = taskKey[Unit]("Publish spyt client & cluster")

    val spytUpdatePythonVersion = taskKey[Unit]("Update versions in data-source/version.py")

    val spytClusterVersionFile = settingKey[File]("Spyt cluster version")
    val spytClientVersionFile = settingKey[File]("Spyt client version")
    val spytClientVersionPyFile = settingKey[File]("Spyt client version")
    val spytSparkVersionFile = settingKey[File]("yandex-spark version")
    val spytSparkVersionPyFile = settingKey[File]("yandex-spark version")

    val releaseClusterCommitMessage = taskKey[String]("")
    val releaseNextClusterCommitMessage = taskKey[String]("")
    val releaseClientCommitMessage = taskKey[String]("")
    val releaseNextClientCommitMessage = taskKey[String]("")
    val releaseAllCommitMessage = taskKey[String]("")
    val releaseNextAllCommitMessage = taskKey[String]("")

    val releaseComponent = settingKey[ReleaseComponent]("")

    val spytReleaseProcess = settingKey[Seq[ReleaseStep]]("")
  }

  sealed abstract class ReleaseComponent(val name: String)

  object ReleaseComponent {
    case object Cluster extends ReleaseComponent("cluster")

    case object Client extends ReleaseComponent("client")

    case object All extends ReleaseComponent("all")

    def fromString(name: String): Option[ReleaseComponent] = Seq(Cluster, Client, All).find(_.name == name)
  }

  import autoImport._

  def updatePythonVersion(spytVersion: String,
                          spytVersionFile: File,
                          sparkVersion: String,
                          sparkVersionFile: File): Unit = {
    val content =
      s"""# This file is autogenerated, don't edit it manually
         |
         |__version__ = "$spytVersion"
         |__spark_version__ = "$sparkVersion"
         |""".stripMargin
    IO.write(spytVersionFile, content)

    val sparkVersionContent = IO.readLines(sparkVersionFile)
      .map {
        case line if line.startsWith("__version__") =>
          s"""__version__ = "$sparkVersion""""
        case line => line
      }
      .mkString("\n")
    IO.write(sparkVersionFile, sparkVersionContent)
  }

  def updatePythonVersionFile(log: Logger, versionFile: File, versions: Map[String, VersionInfo]): Map[String, String] = {
    val effectiveVersions = versions.flatMap { v =>
      if (!v._2.hashChanged) {
        log.info(s"Package ${v._2.packageName}: version hash not changed, skipping update")
        None
      } else if (v._2.nextVersion.isEmpty) {
        log.info(s"Package ${v._2.packageName}: unable to find next version for ${v._2.currentVersion}")
        None
      } else {
        Some(v)
      }
    }

    def findVersions(line: String): Option[(String, VersionInfo)] =
      effectiveVersions.filter { case (_, ver) => ver.hashChanged && ver.nextVersion.isDefined }
        .toSeq
        .find(e => line.startsWith(e._1))

    val content = IO.readLines(versionFile)
      .map { line =>
        findVersions(line) match {
          case Some((prefix, verInfo)) if verInfo.nextVersion.isDefined =>
            log.info(s"Updating package ${verInfo.packageName} version" +
              s" ${verInfo.currentVersion} -> ${verInfo.nextVersion.get} in ${versionFile.getPath}")
            s"""$prefix = "${verInfo.nextVersion.get}""""
          case None =>
            line
        }
      }
      .mkString("\n")
    IO.write(versionFile, content)

    effectiveVersions.mapValues(_.nextVersion.get)
  }

  def gitBranch(submodule: String = ""): Option[String] = {
    val cmd = "git rev-parse --abbrev-ref HEAD"
    val real = if (submodule.isEmpty) cmd else s"git submodule foreach $cmd -- $submodule"
    try Process(real).lineStream.headOption catch { case _: Throwable => None }
  }

  def gitHash(submodule: String = ""): Option[String] = {
    val loc = if (submodule.isEmpty) "HEAD" else s"HEAD:$submodule"
    try Process(s"git rev-parse --short $loc").lineStream.headOption catch { case _: Throwable => None }
  }

  def gitHasUncommited(submodule: String = ""): Boolean = {
    val cmd = "git diff-index --quiet HEAD"
    val real = if (submodule.isEmpty) cmd else s"git submodule foreach $cmd -- $submodule"
    Process(real).! == 1
  }

  def httpQuery(log: Logger, url: String, timeoutSec: Int = 30): Option[String] = {
    val timeout = Duration.ofSeconds(timeoutSec)
    val cli = HttpClient.newBuilder().followRedirects(HttpClient.Redirect.NORMAL).connectTimeout(timeout).build()
    val req: HttpRequest = HttpRequest.newBuilder()
      .timeout(timeout)
      .uri(new URI(url))
      .build()
    log.info(s"Requesting $url")
    val res = cli.send(req, BodyHandlers.ofString())
    if (res.statusCode() != 200) {
      log.error(s"Invalid http response: ${res.statusCode()}: ${res.body()}")
      None
    } else {
      Some(res.body())
    }
  }

  case class PythonVersion(major: Int, minor: Int, patch: Int, beta: Int, dev: Int)
  type PythonVerTuple = (PythonVersion, Option[PythonVersion])

  def listPypiPackageVersions(log: Logger, pythonRegistry: String, packageName: String): Seq[String] = {
    implicit val ord: Ordering[PythonVersion] = Ordering.by {
      v => (v.major, v.minor, v.patch, v.beta, v.dev)
    }

    def verTuple(ver: String): Option[PythonVerTuple] = {
      val splitVer = "^(\\d+)\\.(\\d+).(\\d+)(b(\\d+))?(\\.dev(\\d+))?\\+?(.*)$".r
      ver match {
        case splitVer(maj, min, patch, _, beta, _, dev, local) =>
          val p1 = PythonVersion(
            maj.toInt,
            min.toInt,
            patch.toInt,
            if (beta == null) 0 else beta.toInt,
            if (dev == null) 0 else dev.toInt
          )
          val p2 = if (local != null) verTuple(local).map(_._1) else None
          Some((p1, p2))
        case _ => None
      }
    }
    val extractVer = (s".*<a.*?>$packageName-(.*?)\\.tar\\.gz</a>.*$$").r

    httpQuery(log, s"$pythonRegistry/$packageName")
      .toList
      .flatMap(_.split("\n").toList)
      .flatMap {
        _ match {
          case extractVer(ver) => List((ver, verTuple(ver)))
          case _ => List()
        }
      }
      .sortBy(_._2)
      .reverse
      .map(_._1)
  }

  def latestPyPiVersion(log: Logger, pythonRegistry: String, packageName: String): Option[String] = {
    listPypiPackageVersions(log, pythonRegistry, packageName).headOption
  }

  def ticketFromBranch(branchName: String): Option[String] = {
    val p = "^(\\w+)[ -_](\\d+)[ -_].*$".r

    branchName match {
      case p(q, n) => Some(q.toLowerCase + n)
      case _ => None
    }
  }

  def increaseDevVersion(oldVersion: String, ticket: Option[String], hash: String): Option[String] = {
//    if (oldVersion.endsWith(s".$hash")) { todo:
//      Some(oldVersion)
//    } else {
      val p = "^([0-9.+]+)([ab](\\d+))?(\\.dev(\\d+))?([.+].*)?$".r
      oldVersion match {
        case p(main, beta, _, _, devVer, _) =>
          val newDev = if (devVer == null) 1 else devVer.toInt + 1
          val newBeta = if (beta == null) "" else beta
          val ticketPx = ticket.map(s => s"+$s").getOrElse("")
          Some(s"$main$newBeta.dev$newDev$ticketPx.$hash")
        case _ => None
      }
//    }
  }

  def extractHashFromVer(ver: String): Option[String] = {
    val p = "\\.([a-f0-9]{7})$".r
    ver match {
      case p(hash) => Some(hash)
      case _ => None
    }
  }

  case class VersionInfo(packageName: String, currentVersion: String, currentBranch: String, currentHash: String,
                         versionHash: Option[String]) {
    def hashChanged: Boolean = !versionHash.contains(currentHash)
    def nextVersion: Option[String] = increaseDevVersion(currentVersion, ticketFromBranch(currentBranch), currentHash)
  }

  def packageVersionInfo(log: Logger, pythonRegistry: String, packageName: String,
                         submodule: String): Option[VersionInfo] =
    for {
      curVer <- latestPyPiVersion(log, pythonRegistry, packageName)
      curBranch <- gitBranch(submodule)
      newHash <- gitHash(submodule)
    } yield VersionInfo(packageName, curVer, curBranch, newHash, extractHashFromVer(curVer))


  def updatePythonVersionIfUpdated(log: Logger,
                                   pythonRegistry: String,
                                   versionFile: File,
                                   prefix2package: Map[String, (String, String)]): Map[String, String] = {
    log.info(s"Updating python version file $versionFile")
    val versions = prefix2package.flatMap {
      case (prefix, (packageName, submodule)) =>
        packageVersionInfo(log, pythonRegistry, packageName, submodule).map(vi => (prefix, vi))
    }
    updatePythonVersionFile(log, versionFile, versions)
  }

  def clientSnapshotVersion(pythonVer: String): String = s"$pythonVer-SNAPSHOT"

  def updateClientVersionFile(log: Logger, curVer: String, newVerOpt: Option[String], versionFile: File): Unit = {
    newVerOpt.foreach { newVer =>
      log.info(s"Updating client version from $curVer to $newVer")
      val pythonVer = newVer.replace("-SNAPSHOT", "")
      val content = s"""import spyt.SpytPlugin.autoImport._
                       |
                       |ThisBuild / spytClientVersion := "$newVer"
                       |ThisBuild / spytClientPythonVersion := "$pythonVer"
                       |""".stripMargin
      IO.write(versionFile, content)
    }
  }

  def getClientVersionFromFile(versionFile: File, defaultVer: String): String = {
    val p = "^.*spytClientVersion := \"(.*)\".*$".r
    IO.readLines(versionFile).foreach {
      case p(v) => return v
      case _ => /* ignore */
    }
    defaultVer
  }

  override def projectSettings: Seq[Def.Setting[_]] = super.projectSettings ++ Seq(
    spytClusterVersionFile := baseDirectory.value / "cluster_version.sbt",
    spytClientVersionFile := baseDirectory.value / "client_version.sbt",
    spytSparkVersionFile := baseDirectory.value / "spark_version.sbt",

    pypiRegistry := "https://pypi.yandex-team.ru/simple",

    spytClientVersionPyFile := baseDirectory.value / "data-source" / "src" / "main" / "python" / "spyt" / "version.py",
    spytSparkVersionPyFile := (ThisBuild / sparkVersionPyFile).value,

    spytUpdateClientPythonDevVersion := {
      val vs = updatePythonVersionIfUpdated(streams.value.log, pypiRegistry.value, spytClientVersionPyFile.value,
        Map("__version__" -> ("yandex-spyt", "")))
      val spytVer = vs("__version__")
      StateTransform { st =>
        reapply(Seq(
          (ThisBuild / spytClientVersion) := spytVer,
          (ThisBuild / spytClientPythonVersion) := spytVer
        ), st)
      }
    },

    spytUpdateSparkPythonDevVersion := {
      val curVer = (ThisBuild / spytClientVersion).value
      updatePythonVersionIfUpdated(streams.value.log, pypiRegistry.value, spytSparkVersionPyFile.value,
        Map("__version__" -> ("yandex-pyspark", "spark")))
    },

    spytUpdateAllPythonDevVersions := {
      updatePythonVersionIfUpdated(streams.value.log, pypiRegistry.value, spytSparkVersionPyFile.value,
        Map("__version__" -> ("yandex-pyspark", "spark")))
      updatePythonVersionIfUpdated(streams.value.log, pypiRegistry.value, spytClientVersionPyFile.value,
          Map("__version__" -> ("yandex-spyt", ""), "__spark_version__" -> ("yandex-pyspark", "spark")))
    },

    spytUpdatePythonVersion := spytUpdateAllPythonDevVersions.value,

    spytUpdateClientSnapshotVersion := {
      val curVer = (ThisBuild / spytClientVersion).value
      val vs = updatePythonVersionIfUpdated(streams.value.log, pypiRegistry.value, spytClientVersionPyFile.value,
        Map("__version__" -> ("yandex-spyt", "")))
      val pythonVer = vs("__version__")
      val newVer = Some(clientSnapshotVersion(pythonVer))
      val log = streams.value.log
      updateClientVersionFile(log, curVer, newVer, spytClientVersionFile.value)
      StateTransform { st =>
        reapply(Seq(
          spytClientVersion := newVer.get,
          version := newVer.get,
          (ThisBuild / spytClientVersion) := newVer.get
        ), st)
      }
    },


    spytPublishClusterSnapshot := Def.sequential(
      spytUpdateSparkPythonDevVersion,
      spytUpdateClientSnapshotVersion,
      spytPublishCluster
    ).value,
    spytPublishClientSnapshot := Def.sequential(
      spytUpdateClientPythonDevVersion,
      spytUpdateClientSnapshotVersion,
      spytPublishClient,
    ).value,

    spytPublishAllSnapshots := Def.taskDyn {
      val rebuildSpark = Option(System.getProperty("rebuildSpark")).exists(_.toBoolean)
      if (rebuildSpark) {  
        Def.sequential(
          spytUpdateClientSnapshotVersion,
          spytUpdateAllPythonDevVersions,
          spytPublishAll,
        )
      } else {
        Def.sequential(
          spytUpdateClientSnapshotVersion,
          spytUpdateAllPythonDevVersions,
          spytPublishCluster,
          spytPublishClient,
        )
      }
    }.value,
    releaseClusterCommitMessage := s"Release cluster ${(ThisBuild / spytClusterVersion).value}",
    releaseNextClusterCommitMessage := s"Start cluster ${(ThisBuild / spytClusterVersion).value}",
    releaseClientCommitMessage := s"Release client ${(ThisBuild / spytClientVersion).value}",
    releaseNextClientCommitMessage := s"Start client ${(ThisBuild / spytClientVersion).value}",
    releaseAllCommitMessage := s"Release cluster ${(ThisBuild / spytClusterVersion).value}," +
      s" client ${(ThisBuild / spytClientVersion).value}",
    releaseNextAllCommitMessage := s"Start cluster ${(ThisBuild / spytClusterVersion).value}," +
      s" client ${(ThisBuild / spytClientVersion).value}",
    releaseComponent := {
      Option(System.getProperty("component")).flatMap(ReleaseComponent.fromString).getOrElse(ReleaseComponent.All)
    },
    spytReleaseProcess := {
      releaseComponent.value match {
        case ReleaseComponent.Cluster =>
          Seq[ReleaseStep](
            checkSnapshotDependencies,
            runClean,
            runTest,
            clusterReleaseVersions,
            setReleaseClusterVersion,
            setYtProxies,
            releaseStepTask(spytPublishCluster),
            maybeCommitReleaseClusterVersion,
            maybeSetNextClusterVersion,
            maybeCommitNextClusterVersion,
            maybePushChanges
          )
        case ReleaseComponent.Client =>
          Seq[ReleaseStep](
            checkSnapshotDependencies,
            runClean,
            runTest,
            clientReleaseVersions,
            setReleaseClientVersion,
            releaseStepTask(spytUpdatePythonVersion),
            setYtProxies,
            releaseStepTask(spytPublishClient),
            maybeCommitReleaseClientVersion,
            maybeSetNextClientVersion,
            releaseStepTask(spytUpdatePythonVersion),
            maybeCommitNextClientVersion,
            maybePushChanges
          )
        case ReleaseComponent.All =>
          Seq[ReleaseStep](
            checkSnapshotDependencies,
            runClean,
            runTest,
            allReleaseVersions,
            setReleaseClusterVersion,
            setReleaseClientVersion,
            setReleaseSparkVersion,
            releaseStepTask(spytUpdatePythonVersion),
            setYtProxies,
            setRebuildSpark,
            releaseStepTask(spytPublishAll),
            commitReleaseAllVersion,
            maybeSetNextClusterVersion,
            maybeSetNextClientVersion,
            releaseStepTask(spytUpdatePythonVersion),
            commitNextAllVersion,
            pushChanges
          )
      }
    }
  )
}
