#include "operation.h"

#include "client.h"
#include "file_reader.h"
#include "file_writer.h"
#include "format_hints.h"
#include "init.h"
#include "operation_tracker.h"
#include "retry_heavy_write_request.h"
#include "skiff.h"
#include "yt_poller.h"

#include <mapreduce/yt/common/abortable_registry.h>
#include <mapreduce/yt/common/config.h>
#include <mapreduce/yt/common/helpers.h>
#include <mapreduce/yt/common/wait_proxy.h>

#include <mapreduce/yt/interface/errors.h>
#include <mapreduce/yt/interface/fluent.h>
#include <mapreduce/yt/interface/job_statistics.h>

#include <mapreduce/yt/interface/logging/log.h>

#include <mapreduce/yt/node/serialize.h>

#include <library/yson/writer.h>
#include <library/yson/json_writer.h>

#include <mapreduce/yt/http/requests.h>
#include <mapreduce/yt/http/retry_request.h>

#include <mapreduce/yt/io/job_reader.h>
#include <mapreduce/yt/io/job_writer.h>
#include <mapreduce/yt/io/yamr_table_reader.h>
#include <mapreduce/yt/io/yamr_table_writer.h>
#include <mapreduce/yt/io/node_table_reader.h>
#include <mapreduce/yt/io/node_table_writer.h>
#include <mapreduce/yt/io/proto_table_reader.h>
#include <mapreduce/yt/io/proto_table_writer.h>
#include <mapreduce/yt/io/proto_helpers.h>
#include <mapreduce/yt/io/skiff_table_reader.h>

#include <mapreduce/yt/raw_client/raw_batch_request.h>
#include <mapreduce/yt/raw_client/raw_requests.h>

#include <util/folder/path.h>
#include <util/stream/buffer.h>
#include <util/stream/file.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/printf.h>
#include <util/system/execpath.h>
#include <util/system/mutex.h>
#include <util/system/rwlock.h>
#include <util/system/thread.h>

#include <library/digest/md5/md5.h>

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

constexpr bool USE_GET_OPERATION = true;
constexpr bool USE_NEW_FILE_CACHE = true;

////////////////////////////////////////////////////////////////////////////////

void ApplyFormatHints(
    TFormat* format,
    TMultiFormatDesc::EFormat rowType,
    const TMaybe<TFormatHints>& formatHints);

////////////////////////////////////////////////////////////////////////////////

struct TSmallJobFile
{
    TString FileName;
    TString Data;
};

struct TSimpleOperationIo
{
    TVector<TRichYPath> Inputs;
    TVector<TRichYPath> Outputs;

    TFormat InputFormat;
    TFormat OutputFormat;

    TVector<TSmallJobFile> JobFiles;
};

struct TMapReduceOperationIo
{
    TVector<TRichYPath> Inputs;
    TVector<TRichYPath> MapOutputs;
    TVector<TRichYPath> Outputs;

    TMaybe<TFormat> MapperInputFormat;
    TMaybe<TFormat> MapperOutputFormat;

    TMaybe<TFormat> ReduceCombinerInputFormat;
    TMaybe<TFormat> ReduceCombinerOutputFormat;

    TFormat ReducerInputFormat = TFormat::YsonBinary();
    TFormat ReducerOutputFormat = TFormat::YsonBinary();

    TVector<TSmallJobFile> MapperJobFiles;
    TVector<TSmallJobFile> ReduceCombinerJobFiles;
    TVector<TSmallJobFile> ReducerJobFiles;
};

ui64 RoundUpFileSize(ui64 size)
{
    constexpr ui64 roundUpTo = 4ull << 10;
    return (size + roundUpTo - 1) & ~(roundUpTo - 1);
}

bool IsLocalMode(const TAuth& auth)
{
    static THashMap<TString, bool> localModeMap;
    static TRWMutex mutex;

    {
        TReadGuard guard(mutex);
        auto it = localModeMap.find(auth.ServerName);
        if (it != localModeMap.end()) {
            return it->second;
        }
    }

    bool isLocalMode = false;
    TString localModeAttr("//sys/@local_mode_fqdn");
    if (Exists(auth, TTransactionId(), localModeAttr)) {
        auto fqdn = Get(auth, TTransactionId(), localModeAttr).AsString();
        isLocalMode = (fqdn == TProcessState::Get()->HostName);
    }

    {
        TWriteGuard guard(mutex);
        localModeMap[auth.ServerName] = isLocalMode;
    }

    return isLocalMode;
}

void VerifyHasElements(const TVector<TRichYPath>& paths, const TString& name)
{
    if (paths.empty()) {
        ythrow TApiUsageError() << "no " << name << " table is specified";
    }
}

class TFormatDescImpl
{
public:
    TFormatDescImpl(
        const TAuth& auth,
        const TTransactionId& transactionId,
        const TMultiFormatDesc& formatDesc,
        const TVector<TRichYPath>& tables,
        const TOperationOptions& options,
        ENodeReaderFormat nodeReaderFormat,
        bool allowFormatFromTableAttribute)
        : FormatDesc_(formatDesc)
        , SkiffSchema_(nullptr)
        , FormatFromTableAttribute_()
        , Format_(TNode("NO_FORMAT")) // It will be properly initialized in the constructor body
    {
        switch (FormatDesc_.Format) {
            case TMultiFormatDesc::F_NODE:
                if (nodeReaderFormat != ENodeReaderFormat::Yson) {
                    SkiffSchema_ = TryCreateSkiffSchema(auth, transactionId, tables, options, nodeReaderFormat);
                }
                Format_ = SkiffSchema_
                    ? CreateSkiffFormat(SkiffSchema_)
                    : TFormat::YsonBinary();
                break;
            case TMultiFormatDesc::F_YAMR:
                if (allowFormatFromTableAttribute && options.UseTableFormats_) {
                    FormatFromTableAttribute_ = GetTableFormats(auth, transactionId, tables);
                }
                if (FormatFromTableAttribute_) {
                    Format_ = TFormat(*FormatFromTableAttribute_);
                } else {
                    auto formatNode = TNode("yamr");
                    formatNode.Attributes() = TNode()
                        ("lenval", true)
                        ("has_subkey", true)
                        ("enable_table_index", true);
                    Format_ = TFormat(formatNode);
                }
                break;
            case TMultiFormatDesc::F_PROTO:
                if (TConfig::Get()->UseClientProtobuf) {
                    Format_ = TFormat::YsonBinary();
                } else {
                    Y_ENSURE_EX(!FormatDesc_.ProtoDescriptors.empty(),
                        TApiUsageError() << "Messages for proto format are unknown (empty ProtoDescriptors)");
                    Format_ = TFormat::Protobuf(FormatDesc_.ProtoDescriptors);
                }
                break;
            default:
                Y_FAIL("Unknown format type: %d", static_cast<int>(FormatDesc_.Format));
        }
    }

    const TFormat& GetFormat() const
    {
        return Format_;
    }

    TMaybe<TSmallJobFile> GetFormatConfig(TStringBuf suffix) const {
        switch (FormatDesc_.Format) {
            case TMultiFormatDesc::F_PROTO:
                return TSmallJobFile{TString("proto") + suffix, CreateProtoConfig(FormatDesc_)};
            case TMultiFormatDesc::F_NODE:
                if (SkiffSchema_) {
                    return TSmallJobFile{TString("skiff") + suffix, CreateSkiffConfig(SkiffSchema_)};
                }
                return Nothing();
            default:
                return Nothing();
        }
    }

    TMultiFormatDesc::EFormat GetRowType() const
    {
        return FormatDesc_.Format;
    }

    const TMaybe<TNode>& GetFormatFromTableAttribute() const {
        return FormatFromTableAttribute_;
    }

private:
    TMultiFormatDesc FormatDesc_;
    NSkiff::TSkiffSchemaPtr SkiffSchema_;
    TMaybe<TNode> FormatFromTableAttribute_;
    TFormat Format_;

private:
    static NSkiff::TSkiffSchemaPtr TryCreateSkiffSchema(
        const TAuth& auth,
        const TTransactionId& transactionId,
        const TVector<TRichYPath>& tables,
        const TOperationOptions& options,
        ENodeReaderFormat nodeReaderFormat)
    {
        bool hasInputQuery = options.Spec_.Defined() && options.Spec_->IsMap() && options.Spec_->HasKey("input_query");
        if (hasInputQuery) {
            Y_ENSURE_EX(nodeReaderFormat != ENodeReaderFormat::Skiff,
                TApiUsageError() << "Cannot use Skiff format for operations with 'input_query' in spec");
            return nullptr;
        }
        return CreateSkiffSchemaIfNecessary(
            auth,
            transactionId,
            nodeReaderFormat,
            tables,
            TCreateSkiffSchemaOptions()
                .HasKeySwitch(true));
    }

    static TString CreateProtoConfig(const TMultiFormatDesc& desc)
    {
        Y_VERIFY(desc.Format == TMultiFormatDesc::F_PROTO);

        TString result;
        TStringOutput messageTypeList(result);
        for (const auto& descriptor : desc.ProtoDescriptors) {
            messageTypeList << descriptor->full_name() << Endl;
        }
        return result;
    }

    static TString CreateSkiffConfig(const NSkiff::TSkiffSchemaPtr& schema)
    {
         TString result;
         TStringOutput stream(result);
         TYsonWriter writer(&stream);
         Serialize(schema, &writer);
         return result;
    }
};

TVector<TSmallJobFile> CreateFormatConfig(
    const TFormatDescImpl& inputDesc,
    const TFormatDescImpl& outputDesc)
{
    TVector<TSmallJobFile> result;
    if (auto inputConfig = inputDesc.GetFormatConfig("_input")) {
        result.push_back(std::move(*inputConfig));
    }
    if (auto outputConfig = outputDesc.GetFormatConfig("_output")) {
        result.push_back(std::move(*outputConfig));
    }
    return result;
}

template <typename T>
ENodeReaderFormat NodeReaderFormatFromHintAndGlobalConfig(const TUserJobFormatHintsBase<T>& formatHints)
{
    auto result = TConfig::Get()->NodeReaderFormat;
    if (formatHints.InputFormatHints_ && formatHints.InputFormatHints_->SkipNullValuesForTNode_) {
        Y_ENSURE_EX(
            result != ENodeReaderFormat::Skiff,
            TApiUsageError() << "skiff format doesn't support SkipNullValuesForTNode format hint");
        result = ENodeReaderFormat::Yson;
    }
    return result;
}

template <class TSpec>
TSimpleOperationIo CreateSimpleOperationIo(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TSpec& spec,
    const TOperationOptions& options,
    bool allowSkiff)
{
    VerifyHasElements(spec.Inputs_, "input");
    VerifyHasElements(spec.Outputs_, "output");

    ENodeReaderFormat nodeReaderFormat =
        allowSkiff
        ? NodeReaderFormatFromHintAndGlobalConfig(spec)
        : ENodeReaderFormat::Yson;

    TFormatDescImpl inputDesc(auth, transactionId, spec.GetInputDesc(), spec.Inputs_, options,
        nodeReaderFormat, /* allowFormatFromTableAttribute = */ true);
    TFormatDescImpl outputDesc(auth, transactionId, spec.GetOutputDesc(), spec.Outputs_, options,
        ENodeReaderFormat::Yson, /* allowFormatFromTableAttribute = */ false);

    TFormat inputFormat = inputDesc.GetFormat();
    TFormat outputFormat = outputDesc.GetFormat();

    ApplyFormatHints(&inputFormat, inputDesc.GetRowType(), spec.InputFormatHints_);
    ApplyFormatHints(&outputFormat, outputDesc.GetRowType(), spec.OutputFormatHints_);

    return TSimpleOperationIo {
        CanonizePaths(auth, spec.Inputs_),
        CanonizePaths(auth, spec.Outputs_),

        inputFormat,
        outputFormat,

        CreateFormatConfig(inputDesc, outputDesc)
    };
}

template <class T>
TSimpleOperationIo CreateSimpleOperationIo(
    const TAuth& auth,
    const TSimpleRawOperationIoSpec<T>& spec)
{
    auto getFormatOrDefault = [&] (const TMaybe<TFormat>& maybeFormat, const char* formatName) {
        if (maybeFormat) {
            return *maybeFormat;
        } else if (spec.Format_) {
            return *spec.Format_;
        } else {
            ythrow TApiUsageError() << "Neither " << formatName << "format nor default format is specified for raw operation";
        }
    };

    VerifyHasElements(spec.GetInputs(), "input");
    VerifyHasElements(spec.GetOutputs(), "output");

    return TSimpleOperationIo {
        CanonizePaths(auth, spec.GetInputs()),
        CanonizePaths(auth, spec.GetOutputs()),

        getFormatOrDefault(spec.InputFormat_, "input"),
        getFormatOrDefault(spec.OutputFormat_, "output"),

        TVector<TSmallJobFile>{},
    };
}


////////////////////////////////////////////////////////////////////////////////

struct IItemToUpload
{
    virtual ~IItemToUpload() = default;

    virtual TString CalculateMD5() const = 0;
    virtual THolder<IInputStream> CreateInputStream() const = 0;
};

class TFileToUpload
    : public IItemToUpload
{
public:
    TFileToUpload(const TString& fileName)
        : FileName_(fileName)
    { }

    virtual TString CalculateMD5() const override
    {
        constexpr size_t md5Size = 32;
        TString result;
        result.ReserveAndResize(md5Size);
        MD5::File(~FileName_, result.Detach());
        return result;
    }

    virtual THolder<IInputStream> CreateInputStream() const override
    {
        return MakeHolder<TFileInput>(FileName_);
    }

private:
    TString FileName_;
};

class TDataToUpload
    : public IItemToUpload
{
public:
    TDataToUpload(const TStringBuf& data)
        : Data_(data)
    { }

    virtual TString CalculateMD5() const override
    {
        constexpr size_t md5Size = 32;
        TString result;
        result.ReserveAndResize(md5Size);
        MD5::Data(reinterpret_cast<const unsigned char*>(Data_.Data()), Data_.Size(), result.Detach());
        return result;
    }

    virtual THolder<IInputStream> CreateInputStream() const override
    {
        return MakeHolder<TMemoryInput>(Data_.Data(), Data_.Size());
    }

private:
    TStringBuf Data_;
};


class TJobPreparer
    : private TNonCopyable
{
public:
    TJobPreparer(
        const TAuth& auth,
        const TTransactionId& transactionId,
        const TString& commandLineName,
        const TUserJobSpec& spec,
        IJob* job,
        size_t outputTableCount,
        const TVector<TSmallJobFile>& smallFileList,
        const TOperationOptions& options)
        : Auth_(auth)
        , TransactionId_(transactionId)
        , Spec_(spec)
        , Options_(options)
    {
        auto binaryPath = GetExecPath();
        if (TConfig::Get()->JobBinary) {
            binaryPath = TConfig::Get()->JobBinary;
        }
        if (Spec_.JobBinary_) {
            binaryPath = *Spec_.JobBinary_;
        }
        if (binaryPath == GetExecPath() && GetInitStatus() != EInitStatus::FullInitialization) {
            ythrow yexception() << "NYT::Initialize() must be called prior to any operation";
        }

        CreateStorage();
        auto cypressFileList = CanonizePaths(auth, spec.Files_);
        for (const auto& file : cypressFileList) {
            UseFileInCypress(file);
        }
        for (const auto& localFile : spec.GetLocalFiles()) {
            UploadLocalFile(std::get<0>(localFile), std::get<1>(localFile));
        }
        auto jobStateSmallFile = GetJobState(job);
        if (jobStateSmallFile) {
            UploadSmallFile(*jobStateSmallFile);
        }
        for (const auto& smallFile : smallFileList) {
            UploadSmallFile(smallFile);
        }

        TString binaryPathInsideJob;
        if (!IsLocalMode(auth)) {
            UploadBinary(binaryPath);
            binaryPathInsideJob = "./cppbinary";
        } else {
            binaryPathInsideJob = binaryPath;
        }

        TString jobCommandPrefix = options.JobCommandPrefix_;
        if (!spec.JobCommandPrefix_.empty()) {
            jobCommandPrefix = spec.JobCommandPrefix_;
        }

        TString jobCommandSuffix = options.JobCommandSuffix_;
        if (!spec.JobCommandSuffix_.empty()) {
            jobCommandSuffix = spec.JobCommandSuffix_;
        }

        ClassName_ = TJobFactory::Get()->GetJobName(job);
        Command_ = TStringBuilder() <<
            jobCommandPrefix <<
            (TConfig::Get()->UseClientProtobuf ? "YT_USE_CLIENT_PROTOBUF=1 " : "YT_USE_CLIENT_PROTOBUF=0 ") <<
            binaryPathInsideJob << " " <<
            commandLineName << " " <<
            "\"" << ClassName_ << "\" " <<
            outputTableCount << " " <<
            jobStateSmallFile.Defined() <<
            jobCommandSuffix;
    }

    const TVector<TRichYPath>& GetFiles() const
    {
        return Files_;
    }

    const TString& GetClassName() const
    {
        return ClassName_;
    }

    const TString& GetCommand() const
    {
        return Command_;
    }

    const TUserJobSpec& GetSpec() const
    {
        return Spec_;
    }

    bool ShouldMountSandbox() const
    {
        return TConfig::Get()->MountSandboxInTmpfs || Options_.MountSandboxInTmpfs_;
    }

    ui64 GetTotalFileSize() const
    {
        return TotalFileSize_;
    }

private:
    TAuth Auth_;
    TTransactionId TransactionId_;
    TUserJobSpec Spec_;
    TOperationOptions Options_;

    TVector<TRichYPath> Files_;
    TString ClassName_;
    TString Command_;
    ui64 TotalFileSize_ = 0;

    TString GetFileStorage() const
    {
        return Options_.FileStorage_ ?
            *Options_.FileStorage_ :
            TConfig::Get()->RemoteTempFilesDirectory;
    }

    TYPath GetCachePath() const
    {
        if (USE_NEW_FILE_CACHE) {
            return AddPathPrefix(TStringBuilder() << GetFileStorage() << "/new_cache");
        } else {
            return AddPathPrefix(TStringBuilder() << GetFileStorage() << "/hash");
        }
    }

    void CreateStorage() const
    {
        NYT::NDetail::Create(Auth_, Options_.FileStorageTransactionId_, GetCachePath(), NT_MAP,
            TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true));
    }

    TString UploadToCacheOld(const IItemToUpload& itemToUpload) const
    {
        auto md5Signature = itemToUpload.CalculateMD5();
        Y_VERIFY(md5Signature.size() == 32);

        TString twoDigits = md5Signature.substr(md5Signature.size() - 2);
        Y_VERIFY(twoDigits.size() == 2);

        TString symlinkPath = TStringBuilder() << GetCachePath() <<
            "/" << twoDigits << "/" << md5Signature;

        TString uniquePath = TStringBuilder() << GetFileStorage() <<
            "/" << twoDigits << "/cpp_" << CreateGuidAsString();

        symlinkPath = AddPathPrefix(symlinkPath);
        uniquePath = AddPathPrefix(uniquePath);

        int retryCount = 256;
        for (int attempt = 0; attempt < retryCount; ++attempt) {
            TNode linkAttrs;
            if (Exists(Auth_, Options_.FileStorageTransactionId_, symlinkPath + "&")) {
                try {
                    linkAttrs = Get(Auth_, Options_.FileStorageTransactionId_, symlinkPath + "&/@");
                } catch (TErrorResponse& e) {
                    if (!e.IsResolveError()) {
                        throw;
                    }
                }
            }

            try {
                if (!linkAttrs.IsUndefined()) {
                    if (linkAttrs["type"] == "link" &&
                        (!linkAttrs.HasKey("broken") || !linkAttrs["broken"].AsBool()))
                    {
                        try {
                            NYT::NDetail::Set(Auth_, Options_.FileStorageTransactionId_, symlinkPath + "&/@touched", "true");
                            NYT::NDetail::Set(Auth_, Options_.FileStorageTransactionId_, symlinkPath + "/@touched", "true");
                        } catch (const TErrorResponse& e) {
                            if (!e.IsConcurrentTransactionLockConflict()) {
                                // We might have lock conflict if FileStorageTransactionId is used,
                                // ignore this error, other transaction is probably touched file.
                                throw;
                            }
                        }
                        return symlinkPath;
                    } else {
                        NYT::NDetail::Remove(Auth_, Options_.FileStorageTransactionId_, symlinkPath + "&",
                            TRemoveOptions().Recursive(true).Force(true));
                    }
                }

                NYT::NDetail::Create(Auth_, Options_.FileStorageTransactionId_, uniquePath, NT_FILE,
                    TCreateOptions()
                        .IgnoreExisting(true)
                        .Recursive(true)
                        .Attributes(
                            TNode()
                            ("hash", md5Signature)
                            ("touched", true)));

                {
                    // TODO: use file writer
                    THttpHeader header("PUT", GetWriteFileCommand());
                    header.SetToken(Auth_.Token);
                    header.AddPath(uniquePath);
                    auto streamMaker = [&itemToUpload] () {
                        return itemToUpload.CreateInputStream();
                    };
                    RetryHeavyWriteRequest(Auth_, Options_.FileStorageTransactionId_, header, streamMaker);
                }

                NYT::NDetail::Link(Auth_, Options_.FileStorageTransactionId_, uniquePath, symlinkPath,
                    TLinkOptions()
                        .IgnoreExisting(true)
                        .Recursive(true)
                        .Attributes(TNode()("touched", true)));

            } catch (TErrorResponse& e) {
                if (!e.IsResolveError() || attempt + 1 == retryCount) {
                    throw;
                }
                TWaitProxy::Sleep(TDuration::Seconds(1));
                continue;
            }
            break;
        }
        return symlinkPath;
    }

    class TRetryPolicyIgnoringLockConflicts
        : public TAttemptLimitedRetryPolicy
    {
    public:
        using TAttemptLimitedRetryPolicy::TAttemptLimitedRetryPolicy;
        using TAttemptLimitedRetryPolicy::GetRetryInterval;

        virtual TMaybe<TDuration> GetRetryInterval(const TErrorResponse& e) const override
        {
            if (IsAttemptLimitExceeded()) {
                return Nothing();
            }
            if (e.IsConcurrentTransactionLockConflict()) {
                return TConfig::Get()->RetryInterval;
            }
            return TAttemptLimitedRetryPolicy::GetRetryInterval(e);
        }
    };

    TString UploadToRandomPath(const IItemToUpload& itemToUpload) const
    {
        TString uniquePath = AddPathPrefix(TStringBuilder() << GetFileStorage() << "/cpp_" << CreateGuidAsString());
        Create(Auth_, Options_.FileStorageTransactionId_, uniquePath, NT_FILE, TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true));
        {
            TFileWriter writer(uniquePath, Auth_, Options_.FileStorageTransactionId_);
            itemToUpload.CreateInputStream()->ReadAll(writer);
            writer.Finish();
        }
        return uniquePath;
    }

    TString UploadToCacheUsingApi(const IItemToUpload& itemToUpload) const
    {
        auto md5Signature = itemToUpload.CalculateMD5();
        Y_VERIFY(md5Signature.size() == 32);

        constexpr int LockConflictRetryCount = 30;
        TRetryPolicyIgnoringLockConflicts retryPolicy(LockConflictRetryCount);
        auto maybePath = GetFileFromCache(
            Auth_,
            Options_.FileStorageTransactionId_,
            md5Signature,
            GetCachePath(),
            TGetFileFromCacheOptions(),
            &retryPolicy);
        if (maybePath) {
            return *maybePath;
        }

        TString uniquePath = AddPathPrefix(TStringBuilder() << GetFileStorage() << "/cpp_" << CreateGuidAsString());

        Create(Auth_, Options_.FileStorageTransactionId_, uniquePath, NT_FILE,
            TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true));

        {
            TFileWriter writer(uniquePath, Auth_, Options_.FileStorageTransactionId_,
                TFileWriterOptions().ComputeMD5(true));
            itemToUpload.CreateInputStream()->ReadAll(writer);
            writer.Finish();
        }

        auto cachePath = PutFileToCache(
            Auth_,
            Options_.FileStorageTransactionId_,
            uniquePath,
            md5Signature,
            GetCachePath(),
            TPutFileToCacheOptions(),
            &retryPolicy);

        Remove(Auth_, Options_.FileStorageTransactionId_, uniquePath, TRemoveOptions().Force(true));

        return cachePath;
    }

    TString UploadToCache(const IItemToUpload& itemToUpload) const
    {
        if (USE_NEW_FILE_CACHE) {
            switch (Options_.FileCacheMode_) {
                case TOperationOptions::EFileCacheMode::ApiCommandBased:
                    Y_ENSURE_EX(Options_.FileStorageTransactionId_.IsEmpty(), TApiUsageError() <<
                        "Default cache mode (API command-based) doesn't allow non-default 'FileStorageTransactionId_'");
                    return UploadToCacheUsingApi(itemToUpload);
                case TOperationOptions::EFileCacheMode::CachelessRandomPathUpload:
                    return UploadToRandomPath(itemToUpload);
                default:
                    Y_FAIL("Unknown file cache mode: %d", static_cast<int>(Options_.FileCacheMode_));
            }
        } else {
            return UploadToCacheOld(itemToUpload);
        }
    }

    void UseFileInCypress(const TRichYPath& file)
    {
        if (!Exists(Auth_, TransactionId_, file.Path_)) {
            ythrow yexception() << "File " << file.Path_ << " does not exist";
        }

        if (ShouldMountSandbox()) {
            auto size = Get(Auth_, TransactionId_, file.Path_ + "/@uncompressed_data_size").AsInt64();

            TotalFileSize_ += RoundUpFileSize(static_cast<ui64>(size));
        }
        Files_.push_back(file);
    }

    void UploadLocalFile(const TLocalFilePath& localPath, const TAddLocalFileOptions& options)
    {
        TFsPath fsPath(localPath);
        fsPath.CheckExists();

        TFileStat stat;
        fsPath.Stat(stat);
        bool isExecutable = stat.Mode & (S_IXUSR | S_IXGRP | S_IXOTH);

        auto cachePath = UploadToCache(TFileToUpload(localPath));

        TRichYPath cypressPath(cachePath);
        cypressPath.FileName(options.PathInJob_.GetOrElse(fsPath.Basename()));
        if (isExecutable) {
            cypressPath.Executable(true);
        }

        if (ShouldMountSandbox()) {
            TotalFileSize_ += RoundUpFileSize(stat.Size);
        }

        Files_.push_back(cypressPath);
    }

    void UploadBinary(const TString& binaryPath)
    {
        if (ShouldMountSandbox()) {
            TFsPath path(binaryPath);
            TFileStat stat;
            path.Stat(stat);
            TotalFileSize_ += RoundUpFileSize(stat.Size);
        }

        auto cachePath = UploadToCache(TFileToUpload(binaryPath));
        Files_.push_back(TRichYPath(cachePath)
            .FileName("cppbinary")
            .Executable(true));
    }

    TMaybe<TSmallJobFile> GetJobState(IJob* job)
    {
        TString result;
        {
            TStringOutput output(result);
            job->Save(output);
            output.Finish();
        }
        if (result.empty()) {
            return Nothing();
        } else {
            return TSmallJobFile{"jobstate", result};
        }
    }

    void UploadSmallFile(const TSmallJobFile& smallFile) {
        auto cachePath = UploadToCache(TDataToUpload(smallFile.Data));
        Files_.push_back(TRichYPath(cachePath).FileName(smallFile.FileName));
        if (ShouldMountSandbox()) {
            TotalFileSize_ += RoundUpFileSize(smallFile.Data.Size());
        }
    }
};

////////////////////////////////////////////////////////////////////

TVector<TFailedJobInfo> GetFailedJobInfo(
    const TAuth& auth,
    const TOperationId& operationId,
    const TGetFailedJobInfoOptions& options)
{
    const size_t maxJobCount = options.MaxJobCount_;
    const i64 stderrTailSize = options.StderrTailSize_;

    if (USE_GET_OPERATION) {
        const auto jobList = ListJobs(auth, operationId, TListJobsOptions()
            .State(EJobState::Failed)
            .Limit(maxJobCount))["jobs"].AsList();
        TVector<TFailedJobInfo> result;
        for (const auto& jobNode : jobList) {
            const auto& jobMap = jobNode.AsMap();
            TFailedJobInfo info;
            info.JobId = GetGuid(jobMap.at("id").AsString());
            auto errorIt = jobMap.find("error");
            info.Error = TYtError(errorIt == jobMap.end() ? "unknown error" : errorIt->second);
            if (jobMap.count("stderr_size")) {
                info.Stderr = GetJobStderr(auth, operationId, info.JobId);
                if (info.Stderr.Size() > static_cast<size_t>(stderrTailSize)) {
                    info.Stderr = TString(info.Stderr.Data() + info.Stderr.Size() - stderrTailSize, stderrTailSize);
                }
            }
            result.push_back(std::move(info));
        }
        return result;
    } else {
        const auto operationPath = "//sys/operations/" + GetGuidAsString(operationId);
        const auto jobsPath = operationPath + "/jobs";

        if (!Exists(auth, TTransactionId(), jobsPath)) {
            return {};
        }

        auto jobList = List(auth, TTransactionId(), jobsPath, TListOptions().AttributeFilter(
            TAttributeFilter()
                .AddAttribute("state")
                .AddAttribute("error")));

        TVector<TFailedJobInfo> result;
        for (const auto& job : jobList) {
            if (result.size() >= maxJobCount) {
                break;
            }

            const auto& jobId = job.AsString();
            auto jobPath = jobsPath + "/" + jobId;
            auto& attributes = job.GetAttributes().AsMap();

            const auto stateIt = attributes.find("state");
            if (stateIt == attributes.end() || stateIt->second.AsString() != "failed") {
                continue;
            }
            result.push_back(TFailedJobInfo());
            auto& cur = result.back();
            cur.JobId = GetGuid(job.AsString());

            auto errorIt = attributes.find("error");
            if (errorIt != attributes.end()) {
                cur.Error = TYtError(errorIt->second);
            }

            auto stderrPath = jobPath + "/stderr";
            if (!Exists(auth, TTransactionId(), stderrPath)) {
                continue;
            }

            TRichYPath path(stderrPath);
            i64 stderrSize = Get(auth, TTransactionId(), stderrPath + "/@uncompressed_data_size").AsInt64();
            if (stderrSize > stderrTailSize) {
                path.AddRange(
                    TReadRange().LowerLimit(
                        TReadLimit().Offset(stderrSize - stderrTailSize)));
            }
            IFileReaderPtr reader = new TFileReader(path, auth, TTransactionId());
            cur.Stderr = reader->ReadAll();
        }
        return result;
    }
}

using TDescriptorList = TVector<const ::google::protobuf::Descriptor*>;

TMultiFormatDesc IdentityDesc(const TMultiFormatDesc& multi)
{
    const std::set<const ::google::protobuf::Descriptor*> uniqueDescrs(multi.ProtoDescriptors.begin(), multi.ProtoDescriptors.end());
    if (uniqueDescrs.size() > 1)
    {
        TApiUsageError err;
        err << __LOCATION__ << ": Different input proto descriptors";
        for (const auto& desc : multi.ProtoDescriptors) {
            err << " " << desc->full_name();
        }
        throw err;
    }
    TMultiFormatDesc result;
    result.Format = multi.Format;
    result.ProtoDescriptors.assign(uniqueDescrs.begin(), uniqueDescrs.end());
    return result;
}

//TODO: simplify to lhs == rhs after YT-6967 resolving
bool IsCompatible(const TDescriptorList& lhs, const TDescriptorList& rhs)
{
    return lhs.empty() || rhs.empty() || lhs == rhs;
}

const TMultiFormatDesc& MergeIntermediateDesc(
    const TMultiFormatDesc& lh, const TMultiFormatDesc& rh,
    const char* lhDescr, const char* rhDescr,
    bool allowMultipleDescriptors = false)
{
    if (rh.Format == TMultiFormatDesc::F_NONE) {
        return lh;
    } else if (lh.Format == TMultiFormatDesc::F_NONE) {
        return rh;
    } else if (lh.Format == rh.Format && IsCompatible(lh.ProtoDescriptors, rh.ProtoDescriptors)) {
        const auto& result = rh.ProtoDescriptors.empty() ? lh : rh;
        if (result.ProtoDescriptors.size() > 1 && !allowMultipleDescriptors) {
            ythrow TApiUsageError() << "too many proto descriptors for intermediate table";
        }
        return result;
    } else {
        ythrow TApiUsageError() << "incompatible format specifications: "
            << lhDescr << " {format=" << ui32(lh.Format) << " descrs=" << lh.ProtoDescriptors.size() << "}"
               " and "
            << rhDescr << " {format=" << ui32(rh.Format) << " descrs=" << rh.ProtoDescriptors.size() << "}"
        ;
    }
}

void ApplyFormatHints(TFormat* format, TMultiFormatDesc::EFormat rowType, const TMaybe<TFormatHints>& hints)
{
    switch (rowType) {
        case TMultiFormatDesc::EFormat::F_NODE:
            NYT::NDetail::ApplyFormatHints<TNode>(format, hints);
            break;
        default:
            break;
    }
}

void VerifyIntermediateDesc(const TMultiFormatDesc& desc, const TStringBuf& textDescription)
{
    if (desc.Format != TMultiFormatDesc::F_PROTO) {
        return;
    }
    for (size_t i = 0; i != desc.ProtoDescriptors.size(); ++i) {
        if (!desc.ProtoDescriptors[i]) {
            ythrow TApiUsageError() << "Don't know message type for " << textDescription << "; table index: " << i << " (did you forgot to use Hint* function?)";
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

TOperationId StartOperation(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TString& operationType,
    const TString& ysonSpec,
    bool useStartOperationRequest = false)
{
    THttpHeader header("POST", (useStartOperationRequest ? "start_op" : operationType));
    if (useStartOperationRequest) {
        header.AddParameter("operation_type", operationType);
    }
    header.AddTransactionId(transactionId);
    header.AddMutationId();

    TOperationId operationId = ParseGuidFromResponse(
        RetryRequest(auth, header, TStringBuf(ysonSpec), false, true));

    LOG_INFO("Operation %s started (%s): http://%s/#page=operation&mode=detail&id=%s&tab=details",
        ~GetGuidAsString(operationId), ~operationType, ~auth.ServerName, ~GetGuidAsString(operationId));

    TOperationExecutionTimeTracker::Get()->Start(operationId);

    return operationId;
}

EOperationBriefState CheckOperation(
    const TAuth& auth,
    const TOperationId& operationId)
{
    if (USE_GET_OPERATION) {
        auto attributes = GetOperation(
            auth,
            operationId,
            TGetOperationOptions().AttributeFilter(TOperationAttributeFilter()
                .Add(EOperationAttribute::State)
                .Add(EOperationAttribute::Result)));
        Y_VERIFY(attributes.BriefState);
        if (*attributes.BriefState == EOperationBriefState::Completed) {
            return EOperationBriefState::Completed;
        } else if (*attributes.BriefState == EOperationBriefState::Aborted || *attributes.BriefState == EOperationBriefState::Failed) {
            LOG_ERROR("Operation %s %s (%s)",
                ~GetGuidAsString(operationId),
                ~::ToString(*attributes.BriefState),
                ~ToString(TOperationExecutionTimeTracker::Get()->Finish(operationId)));

            auto failedJobInfoList = GetFailedJobInfo(auth, operationId, TGetFailedJobInfoOptions());

            Y_VERIFY(attributes.Result && attributes.Result->Error);
            ythrow TOperationFailedError(
                *attributes.BriefState == EOperationBriefState::Aborted
                    ? TOperationFailedError::Aborted
                    : TOperationFailedError::Failed,
                operationId,
                *attributes.Result->Error,
                failedJobInfoList);
        }
        return EOperationBriefState::InProgress;
    } else {
        auto opIdStr = GetGuidAsString(operationId);
        auto opPath = Sprintf("//sys/operations/%s", ~opIdStr);
        auto statePath = opPath + "/@state";

        if (!Exists(auth, TTransactionId(), opPath)) {
            ythrow yexception() << "Operation " << opIdStr << " does not exist";
        }

        TString state = Get(auth, TTransactionId(), statePath).AsString();

        if (state == "completed") {
            return EOperationBriefState::Completed;

        } else if (state == "aborted" || state == "failed") {
            LOG_ERROR("Operation %s %s (%s)",
                ~opIdStr,
                ~state,
                ~ToString(TOperationExecutionTimeTracker::Get()->Finish(operationId)));

            auto errorPath = opPath + "/@result/error";
            TYtError ytError(TString("unknown operation error"));
            if (Exists(auth, TTransactionId(), errorPath)) {
                ytError = TYtError(Get(auth, TTransactionId(), errorPath));
            }

            auto failedJobInfoList = GetFailedJobInfo(auth, operationId, TGetFailedJobInfoOptions());

            ythrow TOperationFailedError(
                state == "aborted" ?
                    TOperationFailedError::Aborted :
                    TOperationFailedError::Failed,
                operationId,
                ytError,
                failedJobInfoList);
        }

        return EOperationBriefState::InProgress;
    }
}

void WaitForOperation(
    const TAuth& auth,
    const TOperationId& operationId)
{
    const TDuration checkOperationStateInterval =
        IsLocalMode(auth) ? TDuration::MilliSeconds(100) : TDuration::Seconds(1);

    while (true) {
        auto status = CheckOperation(auth, operationId);
        if (status == EOperationBriefState::Completed) {
            LOG_INFO("Operation %s completed (%s)",
                ~GetGuidAsString(operationId),
                ~ToString(TOperationExecutionTimeTracker::Get()->Finish(operationId)));
            break;
        }
        TWaitProxy::Sleep(checkOperationStateInterval);
    }
}

void AbortOperation(
    const TAuth& auth,
    const TOperationId& operationId)
{
    THttpHeader header("POST", "abort_op");
    header.AddOperationId(operationId);
    header.AddMutationId();
    RetryRequest(auth, header);
}

void CompleteOperation(
    const TAuth& auth,
    const TOperationId& operationId)
{
    THttpHeader header("POST", "complete_op");
    header.AddOperationId(operationId);
    header.AddMutationId();
    RetryRequest(auth, header);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

void BuildUserJobFluently1(
    const TJobPreparer& preparer,
    const TMaybe<TFormat>& inputFormat,
    const TMaybe<TFormat>& outputFormat,
    TFluentMap fluent)
{
    TMaybe<i64> memoryLimit = preparer.GetSpec().MemoryLimit_;
    TMaybe<double> cpuLimit = preparer.GetSpec().CpuLimit_;

    auto tmpfsSize = preparer.GetSpec().ExtraTmpfsSize_.GetOrElse(0LL);
    if (preparer.ShouldMountSandbox()) {
        tmpfsSize += preparer.GetTotalFileSize();
        if (tmpfsSize == 0) {
            // This can be a case for example when it is local mode and we don't upload binary.
            // NOTE: YT doesn't like zero tmpfs size.
            tmpfsSize = RoundUpFileSize(1);
        }
        memoryLimit = memoryLimit.GetOrElse(512ll << 20) + tmpfsSize;
    }

    fluent
        .Item("file_paths").List(preparer.GetFiles())
        .Item("command").Value(preparer.GetCommand())
        .Item("class_name").Value(preparer.GetClassName())
        .DoIf(inputFormat.Defined(), [&] (TFluentMap fluentMap) {
            fluentMap.Item("input_format").Value(inputFormat->Config);
        })
        .DoIf(outputFormat.Defined(), [&] (TFluentMap fluentMap) {
            fluentMap.Item("output_format").Value(outputFormat->Config);
        })
        .DoIf(memoryLimit.Defined(), [&] (TFluentMap fluentMap) {
            fluentMap.Item("memory_limit").Value(*memoryLimit);
        })
        .DoIf(cpuLimit.Defined(), [&] (TFluentMap fluentMap) {
            fluentMap.Item("cpu_limit").Value(*cpuLimit);
        })
        .DoIf(preparer.ShouldMountSandbox(), [&] (TFluentMap fluentMap) {
            fluentMap.Item("tmpfs_path").Value(".");
            fluentMap.Item("tmpfs_size").Value(tmpfsSize);
            fluentMap.Item("copy_files").Value(true);
        });
}

void BuildCommonOperationPart(const TOperationOptions& options, TFluentMap fluent)
{
    const TProcessState* properties = TProcessState::Get();
    const TString& pool = TConfig::Get()->Pool;

    fluent
        .Item("started_by")
        .BeginMap()
            .Item("hostname").Value(properties->HostName)
            .Item("pid").Value(properties->Pid)
            .Item("user").Value(properties->UserName)
            .Item("command").List(properties->CommandLine)
            .Item("wrapper_version").Value(properties->ClientVersion)
        .EndMap()
        .DoIf(!pool.Empty(), [&] (TFluentMap fluentMap) {
            fluentMap.Item("pool").Value(pool);
        })
        .DoIf(options.SecureVault_.Defined(), [&] (TFluentMap fluentMap) {
            Y_ENSURE(options.SecureVault_->IsMap(),
                "SecureVault must be a map node, got " << options.SecureVault_->GetType());
            fluentMap.Item("secure_vault").Value(*options.SecureVault_);
        });
}

template <typename TSpec>
void BuildCommonUserOperationPart(const TSpec& baseSpec, TNode* spec)
{
    if (baseSpec.MaxFailedJobCount_.Defined()) {
        (*spec)["max_failed_job_count"] = *baseSpec.MaxFailedJobCount_;
    }
    if (baseSpec.FailOnJobRestart_.Defined()) {
        (*spec)["fail_on_job_restart"] = *baseSpec.FailOnJobRestart_;
    }
    if (baseSpec.StderrTablePath_.Defined()) {
        (*spec)["stderr_table_path"] = *baseSpec.StderrTablePath_;
    }
    if (baseSpec.CoreTablePath_.Defined()) {
        (*spec)["core_table_path"] = *baseSpec.CoreTablePath_;
    }
}

template <typename TSpec>
void BuildJobCountOperationPart(const TSpec& spec, TNode* nodeSpec)
{
    if (spec.JobCount_.Defined()) {
        (*nodeSpec)["job_count"] = *spec.JobCount_;
    }
    if (spec.DataSizePerJob_.Defined()) {
        (*nodeSpec)["data_size_per_job"] = *spec.DataSizePerJob_;
    }
}

template <typename TSpec>
void BuildPartitionCountOperationPart(const TSpec& spec, TNode* nodeSpec)
{
    if (spec.PartitionCount_.Defined()) {
        (*nodeSpec)["partition_count"] = *spec.PartitionCount_;
    }
    if (spec.PartitionDataSize_.Defined()) {
        (*nodeSpec)["partition_data_size"] = *spec.PartitionDataSize_;
    }
}

template <typename TSpec>
void BuildPartitionJobCountOperationPart(const TSpec& spec, TNode* nodeSpec)
{
    if (spec.PartitionJobCount_.Defined()) {
        (*nodeSpec)["partition_job_count"] = *spec.PartitionJobCount_;
    }
    if (spec.DataSizePerPartitionJob_.Defined()) {
        (*nodeSpec)["data_size_per_partition_job"] = *spec.DataSizePerPartitionJob_;
    }
}

template <typename TSpec>
void BuildMapJobCountOperationPart(const TSpec& spec, TNode* nodeSpec)
{
    if (spec.MapJobCount_.Defined()) {
        (*nodeSpec)["map_job_count"] = *spec.MapJobCount_;
    }
    if (spec.DataSizePerMapJob_.Defined()) {
        (*nodeSpec)["data_size_per_map_job"] = *spec.DataSizePerMapJob_;
    }
}

////////////////////////////////////////////////////////////////////////////////

TString MergeSpec(TNode& dst, const TOperationOptions& options)
{
    MergeNodes(dst["spec"], TConfig::Get()->Spec);
    if (options.Spec_) {
        MergeNodes(dst["spec"], *options.Spec_);
    }
    return NodeToYsonString(dst, YF_BINARY);
}

template <typename TSpec>
void CreateDebugOutputTables(const TSpec& spec, const TAuth& auth)
{
    if (spec.StderrTablePath_.Defined()) {
        NYT::NDetail::Create(auth, TTransactionId(), *spec.StderrTablePath_, NT_TABLE,
            TCreateOptions()
                .IgnoreExisting(true)
                .Recursive(true));
    }
    if (spec.CoreTablePath_.Defined()) {
        NYT::NDetail::Create(auth, TTransactionId(), *spec.CoreTablePath_, NT_TABLE,
            TCreateOptions()
                .IgnoreExisting(true)
                .Recursive(true));
    }
}

void CreateOutputTable(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TRichYPath& path)
{
    Y_ENSURE(path.Path_, "Output table is not set");
    NYT::NDetail::Create(auth, transactionId, path.Path_, NT_TABLE,
        TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true));
}

void CreateOutputTables(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TVector<TRichYPath>& paths)
{
    Y_ENSURE(!paths.empty(), "Output tables are not set");
    for (auto& path : paths) {
        CreateOutputTable(auth, transactionId, path);
    }
}

void CheckInputTablesExist(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TVector<TRichYPath>& paths)
{
    Y_ENSURE(!paths.empty(), "Input tables are not set");
    for (auto& path : paths) {
        Y_ENSURE_EX(NYT::NDetail::Exists(auth, transactionId, path.Path_),
            TApiUsageError() << "Input table '" << path.Path_ << "' doesn't exist");
    }
}

void LogJob(const TOperationId& opId, IJob* job, const char* type)
{
    if (job) {
        LOG_INFO("Operation %s; %s = %s",
            ~GetGuidAsString(opId), type, ~TJobFactory::Get()->GetJobName(job));
    }
}

TString DumpYPath(const TRichYPath& path)
{
    TStringStream stream;
    TYsonWriter writer(&stream, YF_TEXT, YT_NODE);
    Serialize(path, &writer);
    return stream.Str();
}

void LogYPaths(const TOperationId& opId, const TVector<TRichYPath>& paths, const char* type)
{
    for (size_t i = 0; i < paths.size(); ++i) {
        LOG_INFO("Operation %s; %s[%" PRISZT "] = %s",
            ~GetGuidAsString(opId), type, i, ~DumpYPath(paths[i]));
    }
}

void LogYPath(const TOperationId& opId, const TRichYPath& output, const char* type)
{
    LOG_INFO("Operation %s; %s = %s",
        ~GetGuidAsString(opId), type, ~DumpYPath(output));
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

template <typename T>
TOperationId DoExecuteMap(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TSimpleOperationIo& operationIo,
    const TMapOperationSpecBase<T>& spec,
    IJob* mapper,
    const TOperationOptions& options)
{
    if (options.CreateDebugOutputTables_) {
        CreateDebugOutputTables(spec, auth);
    }
    if (options.CreateOutputTables_) {
        CheckInputTablesExist(auth, transactionId, operationIo.Inputs);
        CreateOutputTables(auth, transactionId, operationIo.Outputs);
    }

    TJobPreparer map(
        auth,
        transactionId,
        "--yt-map",
        spec.MapperSpec_,
        mapper,
        operationIo.Outputs.size(),
        operationIo.JobFiles,
        options);

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("mapper").DoMap(std::bind(
            BuildUserJobFluently1,
            std::cref(map),
            operationIo.InputFormat,
            operationIo.OutputFormat,
            std::placeholders::_1))
        .Item("input_table_paths").List(operationIo.Inputs)
        .Item("output_table_paths").List(operationIo.Outputs)
        .DoIf(spec.Ordered_.Defined(), [&] (TFluentMap fluent) {
            fluent.Item("ordered").Value(spec.Ordered_.GetRef());
        })
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    specNode["spec"]["job_io"]["control_attributes"]["enable_row_index"] = TNode(true);
    if (!TConfig::Get()->TableWriter.Empty()) {
        specNode["spec"]["job_io"]["table_writer"] = TConfig::Get()->TableWriter;
    }
    specNode["spec"]["title"] = TNode(map.GetClassName());

    BuildCommonUserOperationPart(spec, &specNode["spec"]);
    BuildJobCountOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "map",
        MergeSpec(specNode, options));

    LogJob(operationId, mapper, "mapper");
    LogYPaths(operationId, operationIo.Inputs, "input");
    LogYPaths(operationId, operationIo.Outputs, "output");

    return operationId;
}

TOperationId ExecuteMap(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TMapOperationSpec& spec,
    IJob* mapper,
    const TOperationOptions& options)
{
    return DoExecuteMap(
        auth,
        transactionId,
        CreateSimpleOperationIo(auth, transactionId, spec, options, /* allowSkiff = */ true),
        spec,
        mapper,
        options);
}

TOperationId ExecuteRawMap(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TRawMapOperationSpec& spec,
    IRawJob* mapper,
    const TOperationOptions& options)
{
    return DoExecuteMap(
        auth,
        transactionId,
        CreateSimpleOperationIo(auth, spec),
        spec,
        mapper,
        options);
}

////////////////////////////////////////////////////////////////////////////////

template <typename T>
TOperationId DoExecuteReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TSimpleOperationIo& operationIo,
    const TReduceOperationSpecBase<T>& spec,
    IJob* reducer,
    const TOperationOptions& options)
{
    if (options.CreateDebugOutputTables_) {
        CreateDebugOutputTables(spec, auth);
    }
    if (options.CreateOutputTables_) {
        CheckInputTablesExist(auth, transactionId, operationIo.Inputs);
        CreateOutputTables(auth, transactionId, operationIo.Outputs);
    }

    TJobPreparer reduce(
        auth,
        transactionId,
        "--yt-reduce",
        spec.ReducerSpec_,
        reducer,
        operationIo.Outputs.size(),
        operationIo.JobFiles,
        options);

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("reducer").DoMap(std::bind(
            BuildUserJobFluently1,
            std::cref(reduce),
            operationIo.InputFormat,
            operationIo.OutputFormat,
            std::placeholders::_1))
        .Item("sort_by").Value(spec.SortBy_)
        .Item("reduce_by").Value(spec.ReduceBy_)
        .DoIf(spec.JoinBy_.Defined(), [&] (TFluentMap fluent) {
            fluent.Item("join_by").Value(spec.JoinBy_.GetRef());
        })
        .DoIf(spec.EnableKeyGuarantee_.Defined(), [&] (TFluentMap fluent) {
            fluent.Item("enable_key_guarantee").Value(spec.EnableKeyGuarantee_.GetRef());
        })
        .Item("input_table_paths").List(operationIo.Inputs)
        .Item("output_table_paths").List(operationIo.Outputs)
        .Item("job_io").BeginMap()
            .Item("control_attributes").BeginMap()
                .Item("enable_key_switch").Value(true)
                .Item("enable_row_index").Value(true)
            .EndMap()
            .DoIf(!TConfig::Get()->TableWriter.Empty(), [&] (TFluentMap fluent) {
                fluent.Item("table_writer").Value(TConfig::Get()->TableWriter);
            })
        .EndMap()
        .Item("title").Value(reduce.GetClassName())
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    BuildCommonUserOperationPart(spec, &specNode["spec"]);
    BuildJobCountOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "reduce",
        MergeSpec(specNode, options));

    LogJob(operationId, reducer, "reducer");
    LogYPaths(operationId, operationIo.Inputs, "input");
    LogYPaths(operationId, operationIo.Outputs, "output");

    return operationId;
}

TOperationId ExecuteReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TReduceOperationSpec& spec,
    IJob* reducer,
    const TOperationOptions& options)
{
    return DoExecuteReduce(
        auth,
        transactionId,
        CreateSimpleOperationIo(auth, transactionId, spec, options, /* allowSkiff = */ false),
        spec,
        reducer,
        options);
}

TOperationId ExecuteRawReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TRawReduceOperationSpec& spec,
    IRawJob* mapper,
    const TOperationOptions& options)
{
    return DoExecuteReduce(
        auth,
        transactionId,
        CreateSimpleOperationIo(auth, spec),
        spec,
        mapper,
        options);
}

////////////////////////////////////////////////////////////////////////////////

template <typename T>
TOperationId DoExecuteJoinReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TSimpleOperationIo& operationIo,
    const TJoinReduceOperationSpecBase<T>& spec,
    IJob* reducer,
    const TOperationOptions& options)
{
    if (options.CreateDebugOutputTables_) {
        CreateDebugOutputTables(spec, auth);
    }
    if (options.CreateOutputTables_) {
        CheckInputTablesExist(auth, transactionId, operationIo.Inputs);
        CreateOutputTables(auth, transactionId, operationIo.Outputs);
    }

    TJobPreparer reduce(
        auth,
        transactionId,
        "--yt-reduce",
        spec.ReducerSpec_,
        reducer,
        operationIo.Outputs.size(),
        operationIo.JobFiles,
        options);

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("reducer").DoMap(std::bind(
            BuildUserJobFluently1,
            std::cref(reduce),
            operationIo.InputFormat,
            operationIo.OutputFormat,
            std::placeholders::_1))
        .Item("join_by").Value(spec.JoinBy_)
        .Item("input_table_paths").List(operationIo.Inputs)
        .Item("output_table_paths").List(operationIo.Outputs)
        .Item("job_io").BeginMap()
            .Item("control_attributes").BeginMap()
                .Item("enable_key_switch").Value(true)
                .Item("enable_row_index").Value(true)
            .EndMap()
            .DoIf(!TConfig::Get()->TableWriter.Empty(), [&] (TFluentMap fluent) {
                fluent.Item("table_writer").Value(TConfig::Get()->TableWriter);
            })
        .EndMap()
        .Item("title").Value(reduce.GetClassName())
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    BuildCommonUserOperationPart(spec, &specNode["spec"]);
    BuildJobCountOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "join_reduce",
        MergeSpec(specNode, options));

    LogJob(operationId, reducer, "reducer");
    LogYPaths(operationId, operationIo.Inputs, "input");
    LogYPaths(operationId, operationIo.Outputs, "output");

    return operationId;
}

TOperationId ExecuteJoinReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TJoinReduceOperationSpec& spec,
    IJob* reducer,
    const TOperationOptions& options)
{
    return DoExecuteJoinReduce(
        auth,
        transactionId,
        CreateSimpleOperationIo(auth, transactionId, spec, options, /* allowSkiff = */ false),
        spec,
        reducer,
        options);
}

TOperationId ExecuteRawJoinReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TRawJoinReduceOperationSpec& spec,
    IRawJob* mapper,
    const TOperationOptions& options)
{
    return DoExecuteJoinReduce(
        auth,
        transactionId,
        CreateSimpleOperationIo(auth, spec),
        spec,
        mapper,
        options);
}

////////////////////////////////////////////////////////////////////////////////

template <typename T>
TOperationId DoExecuteMapReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TMapReduceOperationIo& operationIo,
    const TMapReduceOperationSpecBase<T>& spec,
    IJob* mapper,
    IJob* reduceCombiner,
    IJob* reducer,
    const TOperationOptions& options)
{
    TVector<TRichYPath> allOutputs;
    allOutputs.insert(allOutputs.end(), operationIo.MapOutputs.begin(), operationIo.MapOutputs.end());
    allOutputs.insert(allOutputs.end(), operationIo.Outputs.begin(), operationIo.Outputs.end());

    if (options.CreateDebugOutputTables_) {
        CreateDebugOutputTables(spec, auth);
    }
    if (options.CreateOutputTables_) {
        CheckInputTablesExist(auth, transactionId, operationIo.Inputs);
        CreateOutputTables(auth, transactionId, allOutputs);
    }

    TKeyColumns sortBy = spec.SortBy_;
    TKeyColumns reduceBy = spec.ReduceBy_;

    if (sortBy.Parts_.empty()) {
        sortBy = reduceBy;
    }

    const bool hasMapper = mapper != nullptr;
    const bool hasCombiner = reduceCombiner != nullptr;

    TJobPreparer reduce(
        auth,
        transactionId,
        "--yt-reduce",
        spec.ReducerSpec_,
        reducer,
        operationIo.Outputs.size(),
        operationIo.ReducerJobFiles,
        options);

    TString title;

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .DoIf(hasMapper, [&] (TFluentMap fluent) {
            TJobPreparer map(
                auth,
                transactionId,
                "--yt-map",
                spec.MapperSpec_,
                mapper,
                1 + operationIo.MapOutputs.size(),
                operationIo.MapperJobFiles,
                options);

            fluent.Item("mapper").DoMap(std::bind(
                BuildUserJobFluently1,
                std::cref(map),
                *operationIo.MapperInputFormat,
                *operationIo.MapperOutputFormat,
                std::placeholders::_1));

            title = "mapper:" + map.GetClassName() + " ";
        })
        .DoIf(hasCombiner, [&] (TFluentMap fluent) {
            TJobPreparer combine(
                auth,
                transactionId,
                "--yt-reduce",
                spec.ReduceCombinerSpec_,
                reduceCombiner,
                1,
                operationIo.ReduceCombinerJobFiles,
                options);

            fluent.Item("reduce_combiner").DoMap(std::bind(
                BuildUserJobFluently1,
                std::cref(combine),
                *operationIo.ReduceCombinerInputFormat,
                *operationIo.ReduceCombinerOutputFormat,
                std::placeholders::_1));
            title += "combiner:" + combine.GetClassName() + " ";
        })
        .Item("reducer").DoMap(std::bind(
            BuildUserJobFluently1,
            std::cref(reduce),
            operationIo.ReducerInputFormat,
            operationIo.ReducerOutputFormat,
            std::placeholders::_1))
        .Item("sort_by").Value(sortBy)
        .Item("reduce_by").Value(reduceBy)
        .Item("input_table_paths").List(operationIo.Inputs)
        .Item("output_table_paths").List(allOutputs)
        .Item("mapper_output_table_count").Value(operationIo.MapOutputs.size())
        .Item("map_job_io").BeginMap()
            .Item("control_attributes").BeginMap()
                .Item("enable_row_index").Value(true)
            .EndMap()
            .DoIf(!TConfig::Get()->TableWriter.Empty(), [&] (TFluentMap fluent) {
                fluent.Item("table_writer").Value(TConfig::Get()->TableWriter);
            })
        .EndMap()
        .Item("sort_job_io").BeginMap()
            .Item("control_attributes").BeginMap()
                .Item("enable_key_switch").Value(true)
            .EndMap()
            .DoIf(!TConfig::Get()->TableWriter.Empty(), [&] (TFluentMap fluent) {
                fluent.Item("table_writer").Value(TConfig::Get()->TableWriter);
            })
        .EndMap()
        .Item("reduce_job_io").BeginMap()
            .Item("control_attributes").BeginMap()
                .Item("enable_key_switch").Value(true)
            .EndMap()
            .DoIf(!TConfig::Get()->TableWriter.Empty(), [&] (TFluentMap fluent) {
                fluent.Item("table_writer").Value(TConfig::Get()->TableWriter);
            })
        .EndMap()
        .Item("title").Value(title + "reducer:" + reduce.GetClassName())
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    if (spec.Ordered_) {
        specNode["spec"]["ordered"] = *spec.Ordered_;
    }

    BuildCommonUserOperationPart(spec, &specNode["spec"]);
    BuildMapJobCountOperationPart(spec, &specNode["spec"]);
    BuildPartitionCountOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "map_reduce",
        MergeSpec(specNode, options));

    LogJob(operationId, mapper, "mapper");
    LogJob(operationId, reduceCombiner, "reduce_combiner");
    LogJob(operationId, reducer, "reducer");
    LogYPaths(operationId, operationIo.Inputs, "input");
    LogYPaths(operationId, allOutputs, "output");

    return operationId;
}

TOperationId ExecuteMapReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TMapReduceOperationSpec& spec_,
    IJob* mapper,
    IJob* reduceCombiner,
    IJob* reducer,
    const TMultiFormatDesc& mapperClassOutputDesc,
    const TMultiFormatDesc& reduceCombinerClassInputDesc,
    const TMultiFormatDesc& reduceCombinerClassOutputDesc,
    const TMultiFormatDesc& reducerClassInputDesc,
    const TOperationOptions& options)
{
    TMapReduceOperationSpec spec = spec_;

    const auto& reduceOutputDesc = spec.GetOutputDesc();
    auto reduceInputDesc = MergeIntermediateDesc(reducerClassInputDesc, spec.ReduceInputHintDesc_,
        "spec from reducer CLASS input", "spec from HINT for reduce input");
    VerifyIntermediateDesc(reduceInputDesc, "reducer input");

    auto reduceCombinerOutputDesc = MergeIntermediateDesc(reduceCombinerClassOutputDesc, spec.ReduceCombinerOutputHintDesc_,
        "spec derived from reduce combiner CLASS output", "spec from HINT for reduce combiner output");
    VerifyIntermediateDesc(reduceCombinerOutputDesc, "reduce combiner output");
    auto reduceCombinerInputDesc = MergeIntermediateDesc(reduceCombinerClassInputDesc, spec.ReduceCombinerInputHintDesc_,
        "spec from reduce combiner CLASS input", "spec from HINT for reduce combiner input");
    VerifyIntermediateDesc(reduceCombinerInputDesc, "reduce combiner input");
    auto mapOutputDesc = MergeIntermediateDesc(mapperClassOutputDesc, spec.MapOutputDesc_,
        "spec from mapper CLASS output", "spec from HINT for map output",
        /* allowMultipleDescriptors = */ true);
    VerifyIntermediateDesc(mapOutputDesc, "map output");

    const auto& mapInputDesc = spec.GetInputDesc();

    if (!mapper) {
        //request identity desc only for no mapper cases
        const auto& identityMapInputDesc = IdentityDesc(mapInputDesc);
        if (reduceCombiner) {
            reduceCombinerInputDesc = MergeIntermediateDesc(reduceCombinerInputDesc, identityMapInputDesc,
                "spec derived from reduce combiner CLASS input", "identity spec from mapper CLASS input");
        } else {
            reduceInputDesc = MergeIntermediateDesc(reduceInputDesc, identityMapInputDesc,
                "spec derived from reduce CLASS input", "identity spec from mapper CLASS input" );
        }
    }

    TMapReduceOperationIo operationIo;
    operationIo.Inputs = CanonizePaths(auth, spec.Inputs_);
    operationIo.MapOutputs = CanonizePaths(auth, spec.MapOutputs_);
    operationIo.Outputs = CanonizePaths(auth, spec.Outputs_);

    VerifyHasElements(operationIo.Inputs, "inputs");
    VerifyHasElements(operationIo.Outputs, "outputs");

    auto fixSpec = [&](const TFormatDescImpl& formatDesc) {
        if (auto formatFromTableAttribute = formatDesc.GetFormatFromTableAttribute()) {
            spec.SortBy_.Parts_.clear();
            spec.ReduceBy_.Parts_.clear();

            const auto& attrs = formatFromTableAttribute->GetAttributes();
            const auto& keyColumns = attrs["key_column_names"].AsList();
            for (auto& column : keyColumns) {
                spec.SortBy_.Parts_.push_back(column.AsString());
                spec.ReduceBy_.Parts_.push_back(column.AsString());
            }

            if (attrs.HasKey("subkey_column_names")) {
                const auto& subkeyColumns = attrs["subkey_column_names"].AsList();
                for (const auto& column : subkeyColumns) {
                    spec.SortBy_.Parts_.push_back(column.AsString());
                }
            }
        }
    };

    if (mapper) {
        auto nodeReaderFormat = NodeReaderFormatFromHintAndGlobalConfig(spec.MapperFormatHints_);
        TFormatDescImpl inputDescImpl(auth, transactionId, mapInputDesc, operationIo.Inputs, options,
            nodeReaderFormat, /* allowFormatFromTableAttribute = */ true);
        TFormatDescImpl outputDescImpl(auth, transactionId, mapOutputDesc, operationIo.MapOutputs, options,
            ENodeReaderFormat::Yson, /* allowFormatFromTableAttribute = */ false);
        operationIo.MapperJobFiles = CreateFormatConfig(inputDescImpl, outputDescImpl);
        operationIo.MapperInputFormat = inputDescImpl.GetFormat();
        operationIo.MapperOutputFormat = outputDescImpl.GetFormat();
        ApplyFormatHints(
            operationIo.MapperInputFormat.Get(),
            inputDescImpl.GetRowType(),
            spec.MapperFormatHints_.InputFormatHints_);

        ApplyFormatHints(
            operationIo.MapperOutputFormat.Get(),
            outputDescImpl.GetRowType(),
            spec.MapperFormatHints_.OutputFormatHints_);
    }

    if (reduceCombiner) {
        const bool isFirstStep = !mapper;
        auto inputs = isFirstStep ? operationIo.Inputs : TVector<TRichYPath>();
        TFormatDescImpl inputDescImpl(auth, transactionId, reduceCombinerInputDesc, inputs, options,
            ENodeReaderFormat::Yson, /* allowFormatFromTableAttribute = */ isFirstStep);
        TFormatDescImpl outputDescImpl(auth, transactionId, reduceCombinerOutputDesc, /* tables = */ {}, options,
            ENodeReaderFormat::Yson, /* allowFormatFromTableAttribute = */ false);
        operationIo.ReduceCombinerJobFiles = CreateFormatConfig(inputDescImpl, outputDescImpl);
        operationIo.ReduceCombinerInputFormat = inputDescImpl.GetFormat();
        operationIo.ReduceCombinerOutputFormat = outputDescImpl.GetFormat();

        ApplyFormatHints(
            operationIo.ReduceCombinerInputFormat.Get(),
            inputDescImpl.GetRowType(),
            spec.ReduceCombinerFormatHints_.InputFormatHints_);
        ApplyFormatHints(
            operationIo.ReduceCombinerOutputFormat.Get(),
            outputDescImpl.GetRowType(),
            spec.ReduceCombinerFormatHints_.OutputFormatHints_);

        if (isFirstStep) {
            fixSpec(inputDescImpl);
        }
    }

    const bool isFirstStep = (!mapper && !reduceCombiner);
    auto inputs = isFirstStep ? operationIo.Inputs : TVector<TRichYPath>();
    TFormatDescImpl inputDescImpl(auth, transactionId, reduceInputDesc, inputs, options,
        ENodeReaderFormat::Yson, /* allowFormatFromTableAttribute = */ isFirstStep);
    TFormatDescImpl outputDescImpl(auth, transactionId, reduceOutputDesc, operationIo.Outputs, options,
        ENodeReaderFormat::Yson, /* allowFormatFromTableAttribute = */ false);
    operationIo.ReducerJobFiles = CreateFormatConfig(inputDescImpl, outputDescImpl);
    operationIo.ReducerInputFormat = inputDescImpl.GetFormat();
    operationIo.ReducerOutputFormat = outputDescImpl.GetFormat();
    ApplyFormatHints(
        &operationIo.ReducerInputFormat,
        inputDescImpl.GetRowType(),
        spec.ReducerFormatHints_.InputFormatHints_);

    ApplyFormatHints(
        &operationIo.ReducerOutputFormat,
        outputDescImpl.GetRowType(),
        spec.ReducerFormatHints_.OutputFormatHints_);

    if (isFirstStep) {
        fixSpec(inputDescImpl);
    }

    return DoExecuteMapReduce(
        auth,
        transactionId,
        operationIo,
        spec,
        mapper,
        reduceCombiner,
        reducer,
        options);
}

TOperationId ExecuteRawMapReduce(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TRawMapReduceOperationSpec& spec,
    IRawJob* mapper,
    IRawJob* reduceCombiner,
    IRawJob* reducer,
    const TOperationOptions& options)
{
    TMapReduceOperationIo operationIo;
    operationIo.Inputs = CanonizePaths(auth, spec.GetInputs());
    operationIo.MapOutputs = CanonizePaths(auth, spec.GetMapOutputs());
    operationIo.Outputs = CanonizePaths(auth, spec.GetOutputs());

    VerifyHasElements(operationIo.Inputs, "inputs");
    VerifyHasElements(operationIo.Outputs, "outputs");

    auto getFormatOrDefault = [&] (const TMaybe<TFormat>& maybeFormat, const TMaybe<TFormat> stageDefaultFormat, const char* formatName) {
        if (maybeFormat) {
            return *maybeFormat;
        } else if (stageDefaultFormat) {
            return *stageDefaultFormat;
        } else {
            ythrow TApiUsageError() << "Cannot derive " << formatName;
        }
    };

    if (mapper) {
        operationIo.MapperInputFormat = getFormatOrDefault(spec.MapperInputFormat_, spec.MapperFormat_, "mapper input format");
        operationIo.MapperOutputFormat = getFormatOrDefault(spec.MapperOutputFormat_, spec.MapperFormat_, "mapper output format");
    }

    if (reduceCombiner) {
        operationIo.ReduceCombinerInputFormat = getFormatOrDefault(spec.ReduceCombinerInputFormat_, spec.ReduceCombinerFormat_, "reduce combiner input format");
        operationIo.ReduceCombinerOutputFormat = getFormatOrDefault(spec.ReduceCombinerOutputFormat_, spec.ReduceCombinerFormat_, "reduce combiner output format");
    }

    operationIo.ReducerInputFormat = getFormatOrDefault(spec.ReducerInputFormat_, spec.ReducerFormat_, "reducer input format");
    operationIo.ReducerOutputFormat = getFormatOrDefault(spec.ReducerOutputFormat_, spec.ReducerFormat_, "reducer output format");

    return DoExecuteMapReduce(
        auth,
        transactionId,
        operationIo,
        spec,
        mapper,
        reduceCombiner,
        reducer,
        options);
}

TOperationId ExecuteSort(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TSortOperationSpec& spec,
    const TOperationOptions& options)
{
    auto inputs = CanonizePaths(auth, spec.Inputs_);
    auto output = CanonizePath(auth, spec.Output_);

    if (options.CreateOutputTables_) {
        CheckInputTablesExist(auth, transactionId, inputs);
        CreateOutputTable(auth, transactionId, output);
    }

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("input_table_paths").List(inputs)
        .Item("output_table_path").Value(output)
        .Item("sort_by").Value(spec.SortBy_)
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    BuildPartitionCountOperationPart(spec, &specNode["spec"]);
    BuildPartitionJobCountOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "sort",
        MergeSpec(specNode, options));

    LogYPaths(operationId, inputs, "input");
    LogYPath(operationId, output, "output");

    return operationId;
}

TOperationId ExecuteMerge(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TMergeOperationSpec& spec,
    const TOperationOptions& options)
{
    auto inputs = CanonizePaths(auth, spec.Inputs_);
    auto output = CanonizePath(auth, spec.Output_);

    if (options.CreateOutputTables_) {
        CheckInputTablesExist(auth, transactionId, inputs);
        CreateOutputTable(auth, transactionId, output);
    }

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("input_table_paths").List(inputs)
        .Item("output_table_path").Value(output)
        .Item("mode").Value(::ToString(spec.Mode_))
        .Item("combine_chunks").Value(spec.CombineChunks_)
        .Item("force_transform").Value(spec.ForceTransform_)
        .Item("merge_by").Value(spec.MergeBy_)
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    BuildJobCountOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "merge",
        MergeSpec(specNode, options));

    LogYPaths(operationId, inputs, "input");
    LogYPath(operationId, output, "output");

    return operationId;
}

TOperationId ExecuteErase(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TEraseOperationSpec& spec,
    const TOperationOptions& options)
{
    auto tablePath = CanonizePath(auth, spec.TablePath_);

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("table_path").Value(tablePath)
        .Item("combine_chunks").Value(spec.CombineChunks_)
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    auto operationId = StartOperation(
        auth,
        transactionId,
        "erase",
        MergeSpec(specNode, options));

    LogYPath(operationId, tablePath, "table_path");

    return operationId;
}

TOperationId ExecuteVanilla(
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TVanillaOperationSpec& spec,
    const TOperationOptions& options)
{

    auto addTask = [&](TFluentMap fluent, const TVanillaTask& task) {
        TJobPreparer jobPreparer(
            auth,
            transactionId,
            "--yt-map",
            task.Spec_,
            task.Job_.Get(),
            /* outputTableCount = */ 0,
            /* smallFileList = */ {},
            options);

        fluent
            .Item(task.Name_).BeginMap()
                .Item("job_count").Value(task.JobCount_)
                .Do(std::bind(
                    BuildUserJobFluently1,
                    std::cref(jobPreparer),
                    /* inputFormat = */ Nothing(),
                    /* outputFormat = */ Nothing(),
                    std::placeholders::_1))
            .EndMap();
    };

    TNode specNode = BuildYsonNodeFluently()
    .BeginMap().Item("spec").BeginMap()
        .Item("tasks").DoMapFor(spec.Tasks_, addTask)
        .Do(std::bind(BuildCommonOperationPart, options, std::placeholders::_1))
    .EndMap().EndMap();

    BuildCommonUserOperationPart(spec, &specNode["spec"]);

    auto operationId = StartOperation(
        auth,
        transactionId,
        "vanilla",
        MergeSpec(specNode, options),
        /* useStartOperationRequest = */ true);

    return operationId;
}

////////////////////////////////////////////////////////////////////////////////

class TOperation::TOperationImpl
    : public TThrRefBase
{
public:
    TOperationImpl(const TAuth& auth, const TOperationId& operationId)
        : Auth_(auth)
        , Id_(operationId)
    { }

    const TOperationId& GetId() const;
    NThreading::TFuture<void> Watch(TYtPoller& ytPoller);

    EOperationBriefState GetBriefState();
    TMaybe<TYtError> GetError();
    TJobStatistics GetJobStatistics();
    TMaybe<TOperationBriefProgress> GetBriefProgress();
    void AbortOperation();
    void CompleteOperation();
    TOperationAttributes GetAttributes(const TGetOperationOptions& options);
    void UpdateParameters(const TNode& newParameters);

    void AsyncFinishOperation(TOperationAttributes operationAttributes);
    void FinishWithException(std::exception_ptr exception);
    void UpdateBriefProgress(TMaybe<TOperationBriefProgress> briefProgress);

private:
    void UpdateAttributesAndCall(bool needJobStatistics, std::function<void(const TOperationAttributes&)> func);

    void SyncFinishOperationImpl(const TOperationAttributes&);
    static void* SyncFinishOperationProc(void* );

private:
    const TAuth Auth_;
    const TOperationId Id_;
    TMutex Lock_;
    TMaybe<NThreading::TPromise<void>> CompletePromise_;
    TOperationAttributes Attributes_;
};

////////////////////////////////////////////////////////////////////////////////

class TOperationPollerItem
    : public IYtPollerItem
{
public:
    TOperationPollerItem(::TIntrusivePtr<TOperation::TOperationImpl> operationImpl)
        : OperationAttrPath_("//sys/operations/" + GetGuidAsString(operationImpl->GetId()) + "/@")
        , OperationImpl_(operationImpl)
    { }

    virtual void PrepareRequest(TRawBatchRequest* batchRequest) override
    {
        if (USE_GET_OPERATION) {
            OperationState_ = batchRequest->GetOperation(
                OperationImpl_->GetId(),
                TGetOperationOptions().AttributeFilter(TOperationAttributeFilter()
                    .Add(EOperationAttribute::State)
                    .Add(EOperationAttribute::BriefProgress)
                    .Add(EOperationAttribute::Result)));
        } else {
            // NOTE: we don't request progress/job_statistics here, because it is huge.
            auto nodeFuture = batchRequest->Get(TTransactionId(), OperationAttrPath_,
                TGetOptions().AttributeFilter(
                    TAttributeFilter()
                    .AddAttribute("result")
                    .AddAttribute("state")
                    .AddAttribute("brief_progress")));
            OperationState_ = nodeFuture.Apply([&](const NThreading::TFuture<TNode>& nodeFuture) {
                return ParseOperationAttributes(nodeFuture.GetValue());
            });
        }
    }

    virtual EStatus OnRequestExecuted() override
    {
        try {
            const auto& attributes = OperationState_.GetValue();
            Y_VERIFY(attributes.BriefState);
            if (*attributes.BriefState != EOperationBriefState::InProgress) {
                OperationImpl_->AsyncFinishOperation(attributes);
                return PollBreak;
            } else {
                OperationImpl_->UpdateBriefProgress(attributes.BriefProgress);
            }
        } catch (const TErrorResponse& e) {
            if (!NDetail::IsRetriable(e)) {
                OperationImpl_->FinishWithException(std::current_exception());
                return PollBreak;
            }
        } catch (const yexception& e) {
            OperationImpl_->FinishWithException(std::current_exception());
            return PollBreak;
        }
        return PollContinue;
    }

private:
    const TYPath OperationAttrPath_;
    ::TIntrusivePtr<TOperation::TOperationImpl> OperationImpl_;
    NThreading::TFuture<TOperationAttributes> OperationState_;
};

////////////////////////////////////////////////////////////////////////////////

const TOperationId& TOperation::TOperationImpl::GetId() const
{
    return Id_;
}

NThreading::TFuture<void> TOperation::TOperationImpl::Watch(TYtPoller& ytPoller)
{
    auto guard = Guard(Lock_);

    if (!CompletePromise_) {
        CompletePromise_ = NThreading::NewPromise<void>();
        ytPoller.Watch(::MakeIntrusive<TOperationPollerItem>(this));
    }

    auto operationId = GetId();
    TAbortableRegistry::Get()->Add(operationId, ::MakeIntrusive<TOperationAbortable>(Auth_, operationId));
    auto registry = TAbortableRegistry::Get();
    // We have to own an IntrusivePtr to registry to prevent use-after-free
    auto removeOperation = [registry, operationId](const NThreading::TFuture<void>&) {
        registry->Remove(operationId);
    };
    CompletePromise_->GetFuture().Subscribe(removeOperation);

    return *CompletePromise_;
}

EOperationBriefState TOperation::TOperationImpl::GetBriefState()
{
    EOperationBriefState result = EOperationBriefState::InProgress;
    UpdateAttributesAndCall(false, [&] (const TOperationAttributes& attributes) {
        Y_VERIFY(attributes.BriefState);
        result = *attributes.BriefState;
    });
    return result;
}

TMaybe<TYtError> TOperation::TOperationImpl::GetError()
{
    TMaybe<TYtError> result;
    UpdateAttributesAndCall(false, [&] (const TOperationAttributes& attributes) {
        Y_VERIFY(attributes.Result);
        result = attributes.Result->Error;
    });
    return result;
}

TJobStatistics TOperation::TOperationImpl::GetJobStatistics()
{
    TJobStatistics result;
    UpdateAttributesAndCall(true, [&] (const TOperationAttributes& attributes) {
        if (attributes.Progress) {
            result = attributes.Progress->JobStatistics;
        }
    });
    return result;
}

TMaybe<TOperationBriefProgress> TOperation::TOperationImpl::GetBriefProgress()
{
    {
        auto g = Guard(Lock_);
        if (CompletePromise_.Defined()) {
            // Poller do this job for us
            return Attributes_.BriefProgress;
        }
    }
    TMaybe<TOperationBriefProgress> result;
    UpdateAttributesAndCall(false, [&] (const TOperationAttributes& attributes) {
        result = attributes.BriefProgress;
    });
    return result;
}

void TOperation::TOperationImpl::UpdateBriefProgress(TMaybe<TOperationBriefProgress> briefProgress)
{
    auto g = Guard(Lock_);
    Attributes_.BriefProgress = std::move(briefProgress);
}

void TOperation::TOperationImpl::UpdateAttributesAndCall(bool needJobStatistics, std::function<void(const TOperationAttributes&)> func)
{
    {
        auto g = Guard(Lock_);
        if (Attributes_.BriefState
            && *Attributes_.BriefState != EOperationBriefState::InProgress
            && (!needJobStatistics || Attributes_.Progress))
        {
            func(Attributes_);
            return;
        }
    }

    TOperationAttributes attributes;
    if (USE_GET_OPERATION) {
        attributes = NDetail::GetOperation(
            Auth_,
            Id_,
            TGetOperationOptions().AttributeFilter(TOperationAttributeFilter()
                .Add(EOperationAttribute::Result)
                .Add(EOperationAttribute::Progress)
                .Add(EOperationAttribute::State)
                .Add(EOperationAttribute::BriefProgress)));
    } else {
        auto node = NYT::NDetail::Get(Auth_, TTransactionId(), "//sys/operations/" + GetGuidAsString(Id_) + "/@",
            TGetOptions().AttributeFilter(
                TAttributeFilter()
                .AddAttribute("result")
                .AddAttribute("progress")
                .AddAttribute("state")
                .AddAttribute("brief_progress")));
        attributes = ParseOperationAttributes(node);
    }

    func(attributes);

    Y_ENSURE(attributes.BriefState);
    if (*attributes.BriefState != EOperationBriefState::InProgress) {
        auto g = Guard(Lock_);
        Attributes_ = std::move(attributes);
    }
}

void TOperation::TOperationImpl::FinishWithException(std::exception_ptr e)
{
    CompletePromise_->SetException(e);
}

void TOperation::TOperationImpl::AbortOperation() {
    NYT::NDetail::AbortOperation(Auth_, Id_);
}

void TOperation::TOperationImpl::CompleteOperation() {
    NYT::NDetail::CompleteOperation(Auth_, Id_);
}

TOperationAttributes TOperation::TOperationImpl::GetAttributes(const TGetOperationOptions& options) {
    return NYT::NDetail::GetOperation(Auth_, Id_, options);
}

void TOperation::TOperationImpl::UpdateParameters(const TNode& newParameters)
{
    return NYT::NDetail::UpdateOperationParameters(Auth_, Id_, newParameters);
}

struct TAsyncFinishOperationsArgs
{
    ::TIntrusivePtr<TOperation::TOperationImpl> OperationImpl;
    TOperationAttributes OperationAttributes;
};

void TOperation::TOperationImpl::AsyncFinishOperation(TOperationAttributes operationAttributes)
{
    auto args = new TAsyncFinishOperationsArgs;
    args->OperationImpl = this;
    args->OperationAttributes = std::move(operationAttributes);

    TThread thread(TThread::TParams(&TOperation::TOperationImpl::SyncFinishOperationProc, args).SetName("finish operation"));
    thread.Start();
    thread.Detach();
}

void* TOperation::TOperationImpl::SyncFinishOperationProc(void* pArgs)
{
    THolder<TAsyncFinishOperationsArgs> args(static_cast<TAsyncFinishOperationsArgs*>(pArgs));
    args->OperationImpl->SyncFinishOperationImpl(args->OperationAttributes);
    return nullptr;
}

void TOperation::TOperationImpl::SyncFinishOperationImpl(const TOperationAttributes& attributes)
{
    Y_VERIFY(attributes.BriefState && *attributes.BriefState != EOperationBriefState::InProgress);

    {
        try {
            // `attributes' that came from poller don't have JobStatistics
            // so we call `GetJobStatistics' in order to get it from server
            // and cache inside object.
            GetJobStatistics();
        } catch (const TErrorResponse& ) {
            // But if for any reason we failed to get attributes
            // we complete operation using what we have.
            auto g = Guard(Lock_);
            Attributes_ = attributes;
        }
    }

    if (*attributes.BriefState == EOperationBriefState::Completed) {
        CompletePromise_->SetValue();
    } else if (*attributes.BriefState == EOperationBriefState::Aborted || *attributes.BriefState == EOperationBriefState::Failed) {
        Y_VERIFY(attributes.Result && attributes.Result->Error);
        const auto& error = *attributes.Result->Error;
        LOG_ERROR("Operation %s is `%s' with error: %s",
            ~GetGuidAsString(Id_), ~::ToString(*attributes.BriefState), ~error.FullDescription());
        TString additionalExceptionText;
        TVector<TFailedJobInfo> failedJobStderrInfo;
        if (*attributes.BriefState == EOperationBriefState::Failed) {
            try {
                failedJobStderrInfo = NYT::NDetail::GetFailedJobInfo(Auth_, Id_, TGetFailedJobInfoOptions());
            } catch (const yexception& e) {
                additionalExceptionText = "Cannot get job stderrs: ";
                additionalExceptionText += e.what();
            }
        }
        CompletePromise_->SetException(
            std::make_exception_ptr(
                TOperationFailedError(
                    *attributes.BriefState == EOperationBriefState::Failed
                        ? TOperationFailedError::Failed
                        : TOperationFailedError::Aborted,
                    Id_,
                    error,
                    failedJobStderrInfo) << additionalExceptionText));
    }
}

////////////////////////////////////////////////////////////////////////////////

TOperation::TOperation(TOperationId id, TClientPtr client)
    : Client_(std::move(client))
    , Impl_(::MakeIntrusive<TOperationImpl>(Client_->GetAuth(), id))
{
}

const TOperationId& TOperation::GetId() const
{
    return Impl_->GetId();
}

NThreading::TFuture<void> TOperation::Watch()
{
    return Impl_->Watch(Client_->GetYtPoller());
}

TVector<TFailedJobInfo> TOperation::GetFailedJobInfo(const TGetFailedJobInfoOptions& options)
{
    return NYT::NDetail::GetFailedJobInfo(Client_->GetAuth(), GetId(), options);
}

EOperationBriefState TOperation::GetBriefState()
{
    return Impl_->GetBriefState();
}

TMaybe<TYtError> TOperation::GetError()
{
    return Impl_->GetError();
}

TJobStatistics TOperation::GetJobStatistics()
{
    return Impl_->GetJobStatistics();
}

TMaybe<TOperationBriefProgress> TOperation::GetBriefProgress()
{
    return Impl_->GetBriefProgress();
}

void TOperation::AbortOperation()
{
    Impl_->AbortOperation();
}

void TOperation::CompleteOperation()
{
    Impl_->CompleteOperation();
}

TOperationAttributes TOperation::GetAttributes(const TGetOperationOptions& options)
{
    return Impl_->GetAttributes(options);
}

void TOperation::UpdateParameters(const TNode& newParameters)
{
    return Impl_->UpdateParameters(newParameters);
}

////////////////////////////////////////////////////////////////////////////////

TOperationPtr CreateOperationAndWaitIfRequired(const TOperationId& operationId, TClientPtr client, const TOperationOptions& options)
{
    auto operation = ::MakeIntrusive<TOperation>(operationId, std::move(client));
    if (options.Wait_) {
        auto finishedFuture = operation->Watch();
        TWaitProxy::WaitFuture(finishedFuture);
        finishedFuture.GetValue();
    }
    return operation;
}

////////////////////////////////////////////////////////////////////////////////

void ResetUseClientProtobuf(const char* methodName)
{
    if (!TConfig::Get()->UseClientProtobuf) {
        Cerr << "WARNING! OPTION `TConfig::UseClientProtobuf' IS RESET TO `true'; "
            << "IT CAN DETERIORIATE YOUR CODE PERFORMANCE!!! DON'T USE DEPRECATED METHOD `"
            << "TOperationIOSpec::" << methodName << "' TO AVOID THIS RESET" << Endl;
    }
    TConfig::Get()->UseClientProtobuf = true;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

::TIntrusivePtr<INodeReaderImpl> CreateJobNodeReader()
{
    if (auto schema = NDetail::GetJobInputSkiffSchema()) {
        return new NDetail::TSkiffTableReader(::MakeIntrusive<TJobReader>(0), schema);
    } else {
        return new TNodeTableReader(::MakeIntrusive<TJobReader>(0));
    }
}

::TIntrusivePtr<IYaMRReaderImpl> CreateJobYaMRReader()
{
    return new TYaMRTableReader(::MakeIntrusive<TJobReader>(0));
}

::TIntrusivePtr<IProtoReaderImpl> CreateJobProtoReader()
{
    if (TConfig::Get()->UseClientProtobuf) {
        return new TProtoTableReader(
            ::MakeIntrusive<TJobReader>(0),
            GetJobInputDescriptors());
    } else {
        return new TLenvalProtoTableReader(
            ::MakeIntrusive<TJobReader>(0),
            GetJobInputDescriptors());
    }
}

::TIntrusivePtr<INodeWriterImpl> CreateJobNodeWriter(size_t outputTableCount)
{
    return new TNodeTableWriter(MakeHolder<TJobWriter>(outputTableCount));
}

::TIntrusivePtr<IYaMRWriterImpl> CreateJobYaMRWriter(size_t outputTableCount)
{
    return new TYaMRTableWriter(MakeHolder<TJobWriter>(outputTableCount));
}

::TIntrusivePtr<IProtoWriterImpl> CreateJobProtoWriter(size_t outputTableCount)
{
    if (TConfig::Get()->UseClientProtobuf) {
        return new TProtoTableWriter(
            MakeHolder<TJobWriter>(outputTableCount),
            GetJobOutputDescriptors());
    } else {
        return new TLenvalProtoTableWriter(
            MakeHolder<TJobWriter>(outputTableCount),
            GetJobOutputDescriptors());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
