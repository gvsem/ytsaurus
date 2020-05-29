package ru.yandex.spark.yt.format

import net.sf.saxon.`type`.AtomicType
import org.apache.hadoop.conf.Configuration
import org.apache.hadoop.fs.{FileStatus, Path}
import org.apache.hadoop.mapreduce.{Job, TaskAttemptContext}
import org.apache.log4j.Logger
import org.apache.spark.TaskContext
import org.apache.spark.sql.SparkSession
import org.apache.spark.sql.catalyst.InternalRow
import org.apache.spark.sql.catalyst.expressions.UnsafeProjection
import org.apache.spark.sql.execution.datasources._
import org.apache.spark.sql.execution.vectorized.OnHeapColumnVector
import org.apache.spark.sql.internal.SQLConf
import org.apache.spark.sql.sources.{DataSourceRegister, Filter}
import org.apache.spark.sql.types.StructType
import ru.yandex.spark.yt.format.conf.{SparkYtConfiguration, SparkYtWriteConfiguration, YtTableSparkSettings}
import ru.yandex.spark.yt.fs.YtClientConfigurationConverter.ytClientConfiguration
import ru.yandex.spark.yt.fs.{YtClientProvider, YtPath}
import ru.yandex.spark.yt.serializers.{InternalRowDeserializer, SchemaConverter}
import ru.yandex.spark.yt.wrapper.YtWrapper
import ru.yandex.yt.ytclient.proxy.YtClient

import scala.util.{Failure, Success, Try}

class YtFileFormat extends FileFormat with DataSourceRegister with Serializable {
  override def inferSchema(sparkSession: SparkSession,
                           options: Map[String, String],
                           files: Seq[FileStatus]): Option[StructType] = {
    files.headOption.map { fileStatus =>
      val schemaHint = SchemaConverter.schemaHint(options)
      implicit val client: YtClient = YtClientProvider.ytClient(ytClientConfiguration(sparkSession))
      val path = fileStatus.getPath match {
        case ytPath: YtPath => ytPath.stringPath
        case p => YtPath.basePath(p)
      }
      val schemaTree = YtWrapper.attribute(path, "schema")
      SchemaConverter.sparkSchema(schemaTree, schemaHint)
    }
  }


  override def vectorTypes(requiredSchema: StructType, partitionSchema: StructType, sqlConf: SQLConf): Option[Seq[String]] = {
    Option(Seq.fill(requiredSchema.length)(classOf[OnHeapColumnVector].getName))
  }

  override def buildReaderWithPartitionValues(sparkSession: SparkSession,
                                              dataSchema: StructType,
                                              partitionSchema: StructType,
                                              requiredSchema: StructType,
                                              filters: Seq[Filter],
                                              options: Map[String, String],
                                              hadoopConf: Configuration): PartitionedFile => Iterator[InternalRow] = {
    import ru.yandex.spark.yt.fs.conf._
    val ytClientConf = ytClientConfiguration(hadoopConf)
    val readBatch = supportBatch(sparkSession, requiredSchema, options.get("is_dynamic").exists(_.toBoolean))
    val vectorizedReaderCapacity = hadoopConf.ytConf(SparkYtConfiguration.Read.VectorizedCapacity)

    {
      case ypf: YtPartitionedFile =>
        val log = Logger.getLogger(getClass)
        implicit val yt: YtClient = YtClientProvider.ytClient(ytClientConf)
        val split = YtInputSplit(ypf, requiredSchema)
        log.info(s"Read path: ${split.ytPath}")
        if (readBatch) {
          val ytVectorizedReader = new YtVectorizedReader(vectorizedReaderCapacity, ytClientConf.timeout)
          val iter = new RecordReaderIterator(ytVectorizedReader)
          if (readBatch) ytVectorizedReader.enableBatch()
          Option(TaskContext.get()).foreach(_.addTaskCompletionListener[Unit](_ => iter.close()))
          ytVectorizedReader.initialize(split, null)
          iter.asInstanceOf[Iterator[InternalRow]]
        } else {
          val tableIterator = YtWrapper.readTable(
            split.ytPath,
            InternalRowDeserializer.getOrCreate(requiredSchema),
            ytClientConf.timeout
          )
          val unsafeProjection = UnsafeProjection.create(requiredSchema)
          tableIterator.map(unsafeProjection(_))
        }
    }
  }

  override def prepareWrite(sparkSession: SparkSession,
                            job: Job,
                            options: Map[String, String],
                            dataSchema: StructType): OutputWriterFactory = {
    val ytClientConf = ytClientConfiguration(sparkSession)
    val writeConfiguration = SparkYtWriteConfiguration(sparkSession.sqlContext)
    YtTableSparkSettings.serialize(options, dataSchema, job.getConfiguration)

    new OutputWriterFactory {
      override def getFileExtension(context: TaskAttemptContext): String = ""

      override def newInstance(path: String, dataSchema: StructType, context: TaskAttemptContext): OutputWriter = {
        val transaction = YtOutputCommitter.getWriteTransaction(context.getConfiguration)
        new YtOutputWriter(path, dataSchema, ytClientConf, writeConfiguration, transaction, options)
      }
    }
  }

  override def shortName(): String = "yt"

  override def isSplitable(sparkSession: SparkSession, options: Map[String, String], path: Path): Boolean = {
    true
  }

  override def supportBatch(sparkSession: SparkSession, dataSchema: StructType): Boolean = {
    false
  }

  def supportBatch(sparkSession: SparkSession, dataSchema: StructType, isDynamic: Boolean): Boolean = {
    !isDynamic && dataSchema.forall(f => f.dataType.isInstanceOf[AtomicType])
  }
}
