#pragma once
#ifndef VIRTUAL_INL_H_
#error "Direct inclusion of this file is not allowed, include virtual.h"
// For the sake of sane code completion.
#include "virtual.h"
#endif

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TDefaultConversionTraits
{ };

template <>
class TDefaultConversionTraits<TString>
{
public:
    static TString ConvertKeyToString(const TString& key)
    {
        return key;
    }

    static TString ConvertStringToKey(TStringBuf key)
    {
        return TString(key);
    }
};

template <class T, class TConversionTraits = TDefaultConversionTraits<typename T::key_type>>
class TCollectionBoundMapService
    : public TVirtualMapBase
{
public:
    explicit TCollectionBoundMapService(std::weak_ptr<T> collection)
        : Collection_(collection)
    { }

    virtual i64 GetSize() const override
    {
        if (auto collection = Collection_.lock()) {
            return collection->size();
        }
        return 0;
    }

    virtual std::vector<TString> GetKeys(i64 limit) const override
    {
        std::vector<TString> keys;
        if (auto collection = Collection_.lock()) {
            keys.reserve(limit);
            for (const auto& pair : *collection) {
                if (static_cast<i64>(keys.size()) >= limit) {
                    break;
                }
                keys.emplace_back(TConversionTraits::ConvertKeyToString(pair.first));
            }
        }
        return keys;
    }

    virtual IYPathServicePtr FindItemService(TStringBuf key) const override
    {
        if (auto collection = Collection_.lock()) {
            auto it = collection->find(TConversionTraits::ConvertStringToKey(key));
            if (it == collection->end()) {
                return nullptr;
            }
            return it->second->GetService();
        }
        return nullptr;
    }

private:
    std::weak_ptr<T> Collection_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TCollectionBoundListService
    : public TVirtualListBase
{
public:
    explicit TCollectionBoundListService(std::weak_ptr<T> collection)
        : Collection_(collection)
    { }

    virtual i64 GetSize() const override
    {
        if (auto collection = Collection_.lock()) {
            return collection->size();
        }
        return 0;
    }

    virtual IYPathServicePtr FindItemService(int index) const override
    {
        if (auto collection = Collection_.lock()) {
            YT_VERIFY(0 <= index && index < collection->size());
            return (*collection)[index] ? (*collection)[index]->GetService() : nullptr;
        }
        return nullptr;
    }

private:
    std::weak_ptr<T> Collection_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree
