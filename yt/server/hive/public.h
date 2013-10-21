#pragma once

#include <core/misc/common.h>

#include <ytlib/hive/public.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

class THiveManager;
typedef TIntrusivePtr<THiveManager> THiveManagerPtr;

class TCellDirectory;
typedef TIntrusivePtr<TCellDirectory> TCellDirectoryPtr;

struct TMessage;

class TMailbox;

struct ITransactionManager;
typedef TIntrusivePtr<ITransactionManager> ITransactionManagerPtr;

class TTimestampManager;
typedef TIntrusivePtr<TTimestampManager> TTimestampManagerPtr;

class THiveManagerConfig;
typedef TIntrusivePtr<THiveManagerConfig> THiveManagerConfigPtr;

class TTransactionSupervisorConfig;
typedef TIntrusivePtr<TTransactionSupervisorConfig> TTransactionSupervisorConfigPtr;

class TTimestampManagerConfig;
typedef TIntrusivePtr<TTimestampManagerConfig> TTimestampManagerConfigPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
