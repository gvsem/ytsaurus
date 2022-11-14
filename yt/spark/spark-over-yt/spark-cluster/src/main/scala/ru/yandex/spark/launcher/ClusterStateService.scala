package ru.yandex.spark.launcher

import org.slf4j.LoggerFactory
import ru.yandex.spark.launcher.AutoScaler.{OperationState, SparkState}
import ru.yandex.spark.launcher.ClusterStateService.State
import ru.yandex.spark.yt.wrapper.LogLazy
import ru.yandex.spark.yt.wrapper.discovery.{DiscoveryService, OperationSet}
import ru.yandex.yt.ytclient.proxy.CompoundClient
import ru.yandex.yt.ytclient.proxy.request.UpdateOperationParameters.{ResourceLimits, SchedulingOptions}
import ru.yandex.yt.ytclient.request.{GetOperation, ResumeOperation, SuspendOperation, UpdateOperationParameters}
import ru.yandex.yt.ytclient.request.AbortJob
import tech.ytsaurus.core.GUID

trait ClusterStateService {
  def query: Option[State]
  def setUserSlots(count: Long, stopJobs: Set[String] = Set()): Unit
  def idleJobs: Seq[String]
}

object ClusterStateService extends LogLazy {
  private val log = LoggerFactory.getLogger(ClusterStateService.getClass)

  case class State(operationState: OperationState, sparkState: SparkState, userSlots: Long)

  def apply(discoveryService: DiscoveryService, yt: CompoundClient): ClusterStateService =
    new ClusterStateService {
      val sparkStateService: SparkStateService =
        SparkStateService.sparkStateService(discoveryService.discoverAddress().get.webUiHostAndPort,
          discoveryService.discoverAddress().get.restHostAndPort)

      override def query: Option[State] = {
        log.debug("Querying cluster state")
        discoveryService.operations() match {
          case Some(OperationSet(_, children, _)) =>
            if (children.isEmpty) {
              log.error("Autoscaler operation with empty children ops called")
              None
            } else {
              import ru.yandex.spark.yt.wrapper.discovery.CypressDiscoveryService.YTreeNodeExt
              val workersOp = children.iterator.next() // just single children op supported now
              log.info(s"Worker operation $workersOp")
              val opStats = yt.getOperation(new GetOperation(GUID.valueOf(workersOp))).join()
              val totalJobs = opStats.longAttribute("spec", "tasks", "workers", "job_count")
              val runningJobs = opStats.longAttribute("brief_progress", "jobs", "running")
              val currentUserSlots = opStats.longAttribute("runtime_parameters",
                "scheduling_options_per_pool_tree", "physical", "resource_limits", "user_slots")
              val operationState = for {
                total <- totalJobs
                running <- runningJobs
                slots <- currentUserSlots.orElse(Some(total))
              } yield OperationState(total, running, Math.max(0L, slots - running))
              log.debug(s"operation $workersOp state: $operationState slots: $currentUserSlots")
              val sparkState = sparkStateService.query
              log.info(s"spark state: $sparkState")
              val state = for {
                operation <- operationState
                spark <- sparkState.toOption
                slots <- currentUserSlots.orElse(Some(operation.maxJobs))
              } yield State(operation, spark, slots)
              log.info(s"result state: $state")
              state
            }
          case None =>
            log.error("Autoscaler not supported for single op mode")
            None
        }
      }

      def stopJob(jobId: GUID): Unit = {
        log.info(s"Stopping job $jobId")
        yt.abortJob(new AbortJob(jobId))
      }

      def suspendOperation(operationId: GUID): Unit = {
        log.info(s"Suspending operation $operationId")
        yt.suspendOperation(
          new SuspendOperation.Builder().setOperationId(operationId).setAbortRunningJobs(false).build()
        ).join()
      }

      def resumeOperation(operationId: GUID): Unit = {
        log.info(s"Resuming operation $operationId")
        yt.resumeOperation(new ResumeOperation(operationId)).join()
      }

      def updateUserSlots(operationId: GUID, userSlots: Long): Unit = {
        log.info(s"Updating operation parameters for $operationId: user_slots=$userSlots")
        val req = new UpdateOperationParameters.Builder()
          .setOperationId(operationId)
          .addSchedulingOptions("physical",
            new SchedulingOptions().setResourceLimits(new ResourceLimits().setUserSlots(userSlots)))
          .build()
        yt.updateOperationParameters(req).join()
      }

      override def setUserSlots(slots: Long, stopWorkers: Set[String] = Set()): Unit = {
        val op = GUID.valueOf(discoveryService.operations().get.children.iterator.next())
        if (stopWorkers.nonEmpty)
          try {
            suspendOperation(op)
            stopWorkers.map(GUID.valueOf).foreach(stopJob)
            updateUserSlots(op, slots)
          } finally resumeOperation(op)
        else
          updateUserSlots(op, slots)
      }

      override def idleJobs: Seq[String] =
        sparkStateService.activeWorkers
          .map(sparkStateService.idleWorkers(_).map(_.ytJobId))
          .getOrElse(Seq())
          .flatten
    }
}
