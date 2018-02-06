#include "abortable_http_response.h"

#include <util/system/mutex.h>
#include <util/generic/singleton.h>
#include <util/generic/hash_set.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TAbortableHttpResponseRegistry {
public:
    TOutageId StartOutage(TString urlPattern, size_t responseCount)
    {
        auto g = Guard(Lock_);
        auto id = NextId_++;
        IdToOutage.emplace(id, TOutageEntry{std::move(urlPattern), responseCount});
        return id;
    }

    void StopOutage(TOutageId id)
    {
        auto g = Guard(Lock_);
        IdToOutage.erase(id);
    }

    void Add(TAbortableHttpResponse* response)
    {
        auto g = Guard(Lock_);
        for (auto& p : IdToOutage) {
            auto& entry = p.second;
            if (entry.Counter > 0 && response->GetUrl().find(entry.Pattern) != TString::npos) {
                response->Abort();
                entry.Counter -= 1;
            }
        }
        ResponseList_.PushBack(response);
    }

    void Remove(TAbortableHttpResponse* response)
    {
        auto g = Guard(Lock_);
        response->Unlink();
    }

    static TAbortableHttpResponseRegistry& Get()
    {
        return *Singleton<TAbortableHttpResponseRegistry>();
    }

    int AbortAll(const TString& urlPattern)
    {
        int result = 0;
        for (auto& response : ResponseList_) {
            if (response.GetUrl().find(urlPattern) != TString::npos) {
                response.Abort();
                ++result;
            }
        }
        return result;
    }

private:
    struct TOutageEntry
    {
        TString Pattern;
        size_t Counter;
    };

private:
    TOutageId NextId_ = 0;
    TIntrusiveList<TAbortableHttpResponse> ResponseList_;
    THashMap<TOutageId, TOutageEntry> IdToOutage;
    TMutex Lock_;
};

////////////////////////////////////////////////////////////////////////////////

TAbortableHttpResponse::TOutage::TOutage(TString urlPattern, size_t responseCount, TAbortableHttpResponseRegistry& registry)
    : UrlPattern_(std::move(urlPattern))
    , Registry_(registry)
    , Id_(registry.StartOutage(UrlPattern_, responseCount))
{ }

TAbortableHttpResponse::TOutage::~TOutage()
{
    Stop();
}

void TAbortableHttpResponse::TOutage::Stop()
{
    if (!Stopped_) {
        Registry_.StopOutage(Id_);
        Stopped_ = true;
    }
}

////////////////////////////////////////////////////////////////////////////////

TAbortableHttpResponse::TAbortableHttpResponse(
    IInputStream* socketStream,
    const TString& requestId,
    const TString& hostName,
    const TString& url)
    : THttpResponse(socketStream, requestId, hostName)
    , Url_(url)
{
    TAbortableHttpResponseRegistry::Get().Add(this);
}

TAbortableHttpResponse::~TAbortableHttpResponse()
{
    TAbortableHttpResponseRegistry::Get().Remove(this);
}

size_t TAbortableHttpResponse::DoRead(void* buf, size_t len)
{
    if (Aborted_) {
        ythrow TAbortedForTestPurpose();
    }
    return THttpResponse::DoRead(buf, len);
}

size_t TAbortableHttpResponse::DoSkip(size_t len)
{
    if (Aborted_) {
        ythrow TAbortedForTestPurpose();
    }
    return THttpResponse::DoSkip(len);
}

void TAbortableHttpResponse::Abort()
{
    Aborted_ = true;
}

int TAbortableHttpResponse::AbortAll(const TString& urlPattern)
{
    return TAbortableHttpResponseRegistry::Get().AbortAll(urlPattern);
}

TAbortableHttpResponse::TOutage TAbortableHttpResponse::StartOutage(const TString& urlPattern, size_t responseCount)
{
    return TOutage(urlPattern, responseCount, TAbortableHttpResponseRegistry::Get());
}

const TString& TAbortableHttpResponse::GetUrl() const
{
    return Url_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
