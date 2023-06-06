package spyt

import sbt.Keys.baseDirectory
import sbt.{IO, SettingKey, State, StateTransform, TaskKey, ThisBuild}
import sbtrelease.ReleasePlugin.autoImport._
import sbtrelease.ReleaseStateTransformations.reapply
import sbtrelease.Utilities._
import sbtrelease.Versions
import spyt.SpytPlugin.autoImport._
import spyt.SpytSnapshot.SnapshotVersion

import java.io.File

object ReleaseUtils {
  val isTeamCity: Boolean = sys.env.get("IS_TEAMCITY").contains("1")

  lazy val logClientVersion: ReleaseStep = { st: State =>
    st.log.info(s"Client scala version: ${st.extract.get(spytClientVersion)}")
    st.log.info(s"Client python version: ${st.extract.get(spytClientPythonVersion)}")
    st
  }
  lazy val logClusterVersion: ReleaseStep = { st: State =>
    st.log.info(s"Cluster version: ${st.extract.get(spytClusterVersion)}")
    st
  }
  lazy val logSparkForkVersion: ReleaseStep = { st: State =>
    st.log.info(s"Spark fork scala version: ${st.extract.get(spytSparkVersion)}")
    st.log.info(s"Spark fork python version: ${st.extract.get(spytSparkPythonVersion)}")
    st
  }
  lazy val dumpVersions: ReleaseStep = { st: State =>
    val clientScalaVersion = st.extract.get(spytClientVersion)
    val clientPythonVersion = st.extract.get(spytClientPythonVersion)
    val clusterVersion = st.extract.get(spytClusterVersion)
    val sparkScalaVersion = st.extract.get(spytSparkVersion)
    val sparkPythonVersion = st.extract.get(spytSparkPythonVersion)
    st.log.info(s"Client scala version dump: $clientScalaVersion")
    st.log.info(s"Client python version dump: $clientPythonVersion")
    st.log.info(s"Cluster version dump: $clusterVersion")
    st.log.info(s"Spark fork scala version dump: $sparkScalaVersion")
    st.log.info(s"Spark fork python version dump: $sparkPythonVersion")
    if (!publishYtEnabled) {
      dumpVersionsToBuildDirectory(
        Map("scala" -> clientScalaVersion, "python" -> clientPythonVersion),
        st.extract.get(baseDirectory), "client_version.json"
      )
      dumpVersionsToBuildDirectory(
        Map("scala" -> clusterVersion),
        st.extract.get(baseDirectory), "cluster_version.json"
      )
      dumpVersionsToBuildDirectory(
        Map("scala" -> sparkScalaVersion, "python" -> sparkPythonVersion),
        st.extract.get(baseDirectory), "spark_version.json"
      )
    }
    st
  }

  lazy val updateSparkForkDependency: ReleaseStep = { st: State =>
    st.log.info(s"Updating spark fork dependency version to ${st.extract.get(spytSparkVersion)}")
    updateSparkDependencyVersion(st.extract.get(spytSparkVersion), st.extract.get(spytSparkDependencyFile))
    val newSparkDependency = st.extract.get(spytSparkForkDependency)
      .map(module => module.withRevision(st.extract.get(spytSparkVersion)))
    reapply(Seq(
      ThisBuild / spytSparkForkDependency := newSparkDependency
    ), st)
  }
  lazy val setSparkForkSnapshotVersionMvn: ReleaseStep = { st: State =>
    writeVersionToPom(st.extract.get(spytSparkVersion), st.extract.get(spytSparkPomFile))
    st
  }
  lazy val unsetSparkForkSnapshotVersionMvn: ReleaseStep = { st: State =>
    val mainVersion = SnapshotVersion.parse(st.extract.get(spytSparkVersion)).main
    writeVersionToPom(mainVersion, st.extract.get(spytSparkPomFile))
    st
  }

  def updatePythonVersion(spytPythonVersion: String,
                          spytScalaVersion: String,
                          spytVersionFile: File,
                          sparkVersion: String,
                          sparkVersionFile: File): Unit = {
    val content =
      s"""# This file is autogenerated, don't edit it manually
         |
         |__version__ = "$spytPythonVersion"
         |__scala_version__ = "$spytScalaVersion"
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

  def updateSparkDependencyVersion(version: String, file: File): Unit = {
    val line =
      s"""package spyt
         |
         |object SparkForkVersion {
         |  val sparkForkVersion = "$version"
         |}
         |""".stripMargin
    IO.writeLines(file, Seq(line))
  }

  def writeVersionToPom(version: String, file: File): Unit = {
    val lines = IO.readLines(file).map {
      case line if line.contains("fork.version") =>
        s"    <fork.version>$version</fork.version>"
      case line => line
    }
    IO.writeLines(file, lines)
  }

  def writeVersion(versions: Seq[(SettingKey[String], String)],
                   file: File): Unit = {
    val versionStr =
      s"""import spyt.SpytPlugin.autoImport._
         |
         |${versions.map { case (k, v) => s"""ThisBuild / ${k.key} := "$v"""" }.mkString("\n")}""".stripMargin
    IO.writeLines(file, Seq(versionStr))
  }


  def setVersion(versions: SettingKey[Versions],
                 spytVersions: Seq[(SettingKey[String], Versions => String)],
                 fileSetting: SettingKey[File]): ReleaseStep = { st: State =>
    val vs = st.get(versions.key)
      .getOrElse(sys.error("No versions are set! Was this release part executed before inquireVersions?"))
    val selected = spytVersions.map(v => v._1 -> v._2.apply(vs))

    st.log.info(s"Setting ${selected.map { case (k, v) => s"${k.key} to $v" }.mkString(", ")}")

    val file = st.extract.get(fileSetting)
    writeVersion(selected, file)

    reapply(selected.map { case (k, v) => ThisBuild / k := v }, st)
  }

  def maybeSetVersion(versions: SettingKey[Versions],
                      spytVersions: Seq[(SettingKey[String], Versions => String)],
                      fileSetting: SettingKey[File]): ReleaseStep = {
    if (isTeamCity) {
      identity[State](_)
    } else {
      setVersion(versions, spytVersions, fileSetting)
    }
  }

  def runProcess(state: State, process: Seq[ReleaseStep]*): State = {
    process.foldLeft(state) { case (processState, nextProcess) =>
      nextProcess.foldLeft(processState) { case (stepState, nextStep) =>
        StateTransform(nextStep).transform(stepState)
      }
    }
  }

  def deploySparkFork: TaskKey[Unit] = {
    if (!Option(System.getProperty("publishRepo")).exists(_.toBoolean)) {
      spytMvnInstallSparkFork
    } else {
      spytMvnDeploySparkFork
    }
  }
}
