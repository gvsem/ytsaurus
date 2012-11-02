#pragma once

#include "public.h"

#include <ytlib/yson/public.h>

#include <ytlib/misc/mpl.h>
#include <ytlib/misc/property.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/error.h>

#include <ytlib/actions/bind.h>
#include <ytlib/actions/callback.h>

namespace NYT {
namespace NConfig {

// Introduces Serialize function family into current scope.
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

struct IParameter
    : public TRefCounted
{
    // node can be NULL
    virtual void Load(NYTree::INodePtr node, const NYPath::TYPath& path) = 0;
    virtual void Validate(const NYPath::TYPath& path) const = 0;
    virtual void Save(NYson::IYsonConsumer* consumer) const = 0;
    virtual bool IsPresent() const = 0;
};

typedef TIntrusivePtr<IParameter> IParameterPtr;

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TParameter
    : public IParameter
{
public:
    /*!
     * \note Must throw exception for incorrect data
     */
    typedef TCallback<void(const T&)> TValidator;
    typedef typename TNullableTraits<T>::TValueType TValueType;

    explicit TParameter(T& parameter);

    virtual void Load(NYTree::INodePtr node, const NYPath::TYPath& path);
    virtual void Validate(const NYPath::TYPath& path) const;
    virtual void Save(NYson::IYsonConsumer* consumer) const;
    virtual bool IsPresent() const;

public: // for users
    TParameter& Default(const T& defaultValue = T());
    TParameter& Default(T&& defaultValue);
    TParameter& DefaultNew();
    TParameter& CheckThat(TValidator validator);
    TParameter& GreaterThan(TValueType value);
    TParameter& GreaterThanOrEqual(TValueType value);
    TParameter& LessThan(TValueType value);
    TParameter& LessThanOrEqual(TValueType value);
    TParameter& InRange(TValueType lowerBound, TValueType upperBound);
    TParameter& NonEmpty();
    
private:
    T& Parameter;
    bool HasDefaultValue;
    std::vector<TValidator> Validators;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NConfig

////////////////////////////////////////////////////////////////////////////////

class TYsonSerializable
    : public TRefCounted
{
public:
    TYsonSerializable();

    void Load(NYTree::INodePtr node, bool validate = true, const NYPath::TYPath& path = "");
    void Validate(const NYPath::TYPath& path = "") const;

    void Save(NYson::IYsonConsumer* consumer) const;

    DEFINE_BYVAL_RW_PROPERTY(bool, KeepOptions);
    NYTree::IMapNodePtr GetOptions() const;

    std::vector<Stroka> GetRegisteredKeys() const;

protected:
    virtual void DoValidate() const;
    virtual void OnLoaded();

    template <class T>
    NConfig::TParameter<T>& Register(const Stroka& parameterName, T& value);

private:
    template <class T>
    friend class TParameter;

    typedef yhash_map<Stroka, NConfig::IParameterPtr> TParameterMap;
    
    TParameterMap Parameters;
    NYTree::IMapNodePtr Options;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
TIntrusivePtr<T> CloneYsonSerializable(TIntrusivePtr<T> obj);

void Serialize(const TYsonSerializable& value, NYson::IYsonConsumer* consumer);
void Deserialize(TYsonSerializable& value, NYTree::INodePtr node);

template <class T>
TIntrusivePtr<T> UpdateYsonSerializable(
    TIntrusivePtr<T> obj,
    NYTree::INodePtr patch);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define YSON_SERIALIZABLE_INL_H_
#include "yson_serializable-inl.h"
#undef YSON_SERIALIZABLE_INL_H_
