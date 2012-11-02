#pragma once

#include "ephemeral_node_factory.h"
#include "yson_producer.h"

#include <ytlib/yson/yson_writer.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/mpl.h>
#include <ytlib/misc/guid.h>

namespace NYT {

class TYsonSerializable;

} // namespace NYT

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class T>
NYson::EYsonType GetYsonType(const T&);
NYson::EYsonType GetYsonType(const TYsonString& yson);
NYson::EYsonType GetYsonType(const TYsonInput& input);
NYson::EYsonType GetYsonType(const TYsonProducer& producer);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void WriteYson(
    TOutputStream* output,
    const T& value,
    NYson::EYsonType type,
    NYson::EYsonFormat format = NYson::EYsonFormat::Binary);

template <class T>
void WriteYson(
    TOutputStream* output,
    const T& value,
    NYson::EYsonFormat format = NYson::EYsonFormat::Binary);

template <class T>
void WriteYson(
    const TYsonOutput& output,
    const T& value,
    NYson::EYsonFormat format = NYson::EYsonFormat::Binary);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Serialize(T* value, NYson::IYsonConsumer* consumer);

template <class T>
void Serialize(const TIntrusivePtr<T>& value, NYson::IYsonConsumer* consumer);

void Serialize(short value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned short value, NYson::IYsonConsumer* consumer);
void Serialize(int value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned int value, NYson::IYsonConsumer* consumer);
void Serialize(long value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned long value, NYson::IYsonConsumer* consumer);
void Serialize(long long value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned long long value, NYson::IYsonConsumer* consumer);

// double
void Serialize(double value, NYson::IYsonConsumer* consumer);

// Stroka
void Serialize(const Stroka& value, NYson::IYsonConsumer* consumer);

// TStringBuf
void Serialize(const TStringBuf& value, NYson::IYsonConsumer* consumer);

// const char*
void Serialize(const char* value, NYson::IYsonConsumer* consumer);

// bool
void Serialize(bool value, NYson::IYsonConsumer* consumer);

// char
void Serialize(char value, NYson::IYsonConsumer* consumer);

// TDuration
void Serialize(TDuration value, NYson::IYsonConsumer* consumer);

// TInstant
void Serialize(TInstant value, NYson::IYsonConsumer* consumer);

// TGuid
void Serialize(const TGuid& value, NYson::IYsonConsumer* consumer);

// TInputStream
void Serialize(TInputStream& input, NYson::IYsonConsumer* consumer);

// TEnumBase
template <class T>
void Serialize(
    T value,
    NYson::IYsonConsumer* consumer,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, TEnumBase<T>&>, int>::TType = 0);

// TNullable
template <class T>
void Serialize(const TNullable<T>& value, NYson::IYsonConsumer* consumer);

// std::vector
template <class T>
void Serialize(const std::vector<T>& value, NYson::IYsonConsumer* consumer);

// std::vector
template <class T>
void Serialize(const std::vector<T>& value, NYson::IYsonConsumer* consumer);

// yhash_set
template <class T>
void Serialize(const yhash_set<T>& value, NYson::IYsonConsumer* consumer);

// yhash_map
template <class T>
void Serialize(const yhash_map<Stroka, T>& value, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Deserialize(TIntrusivePtr<T>& value, INodePtr node);

template <class T>
void Deserialize(TAutoPtr<T>& value, INodePtr node);

// i64
void Deserialize(i64& value, INodePtr node);

// ui64
void Deserialize(ui64& value, INodePtr node);

// i32
void Deserialize(i32& value, INodePtr node);

// ui32
void Deserialize(ui32& value, INodePtr node);

// i16
void Deserialize(i16& value, INodePtr node);

// ui16
void Deserialize(ui16& value, INodePtr node);

// double
void Deserialize(double& value, INodePtr node);

// Stroka
void Deserialize(Stroka& value, INodePtr node);

// bool
void Deserialize(bool& value, INodePtr node);

// char
void Deserialize(char& value, INodePtr node);

// TDuration
void Deserialize(TDuration& value, INodePtr node);

// TInstant
void Deserialize(TInstant& value, INodePtr node);

// TGuid
void Deserialize(TGuid& value, INodePtr node);

// TEnumBase
template <class T>
void Deserialize(
    T& value,
    INodePtr node,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, TEnumBase<T>&>, int>::TType = 0);

// TNullable
template <class T>
void Deserialize(TNullable<T>& value, INodePtr node);

// std::vector
template <class T>
void Deserialize(std::vector<T>& value, INodePtr node);

// std::vector
template <class T>
void Deserialize(std::vector<T>& value, INodePtr node);

// yhash_set
template <class T>
void Deserialize(yhash_set<T>& value, INodePtr node);

// yhash_map
template <class T>
void Deserialize(yhash_map<Stroka, T>& value, INodePtr node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

#define SERIALIZE_INL_H_
#include "serialize-inl.h"
#undef SERIALIZE_INL_H_
