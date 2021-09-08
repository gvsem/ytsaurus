package rpcclient

import (
	"context"
	"io"

	"a.yandex-team.ru/library/go/core/xerrors"
	"a.yandex-team.ru/library/go/ptr"
	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/proto/client/api/rpc_proxy"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
)

// Encoder is adapter between typed and untyped layer of API.
type Encoder struct {
	StartCall func() *Call

	Invoke        CallInvoker
	InvokeReadRow ReadRowInvoker
}

func (e *Encoder) newCall(method Method, req Request, attachments [][]byte) *Call {
	call := e.StartCall()
	call.Method = method
	call.Req = req
	call.Attachments = attachments
	call.CallID = guid.New()
	return call
}

func (e *Encoder) CreateNode(
	ctx context.Context,
	path ypath.YPath,
	typ yt.NodeType,
	opts *yt.CreateNodeOptions,
) (id yt.NodeID, err error) {
	if opts == nil {
		opts = &yt.CreateNodeOptions{}
	}

	attrs, err := convertAttributes(opts.Attributes)
	if err != nil {
		err = xerrors.Errorf("unable to serialize attributes: %w", err)
		return
	}

	objectType, err := convertObjectType(typ)
	if err != nil {
		return
	}

	req := &rpc_proxy.TReqCreateNode{
		Path:                 ptr.String(path.YPath().String()),
		Type:                 ptr.Int32(int32(objectType)),
		Attributes:           attrs,
		Recursive:            &opts.Recursive,
		Force:                &opts.Force,
		IgnoreExisting:       &opts.IgnoreExisting,
		LockExisting:         nil, // todo check unimportant
		IgnoreTypeMismatch:   nil, // todo check unimportant
		TransactionalOptions: convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:  convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodCreateNode, NewCreateNodeRequest(req), nil)

	var rsp rpc_proxy.TRspCreateNode
	if err = e.Invoke(ctx, call, &rsp); err != nil {
		return
	}

	id = makeNodeID(rsp.NodeId)
	return
}

func (e *Encoder) CreateObject(
	ctx context.Context,
	typ yt.NodeType,
	opts *yt.CreateObjectOptions,
) (id yt.NodeID, err error) {
	if opts == nil {
		opts = &yt.CreateObjectOptions{}
	}

	attrs, err := convertAttributes(opts.Attributes)
	if err != nil {
		err = xerrors.Errorf("unable to serialize attributes: %w", err)
		return
	}

	objectType, err := convertObjectType(typ)
	if err != nil {
		return
	}

	req := &rpc_proxy.TReqCreateObject{
		Type:           ptr.Int32(int32(objectType)),
		Attributes:     attrs,
		IgnoreExisting: &opts.IgnoreExisting,
	}

	call := e.newCall(MethodCreateObject, NewCreateObjectRequest(req), nil)

	var rsp rpc_proxy.TRspCreateObject
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	id = makeNodeID(rsp.GetObjectId())
	return
}

func (e *Encoder) NodeExists(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.NodeExistsOptions,
) (ok bool, err error) {
	if opts == nil {
		opts = &yt.NodeExistsOptions{}
	}

	req := &rpc_proxy.TReqExistsNode{
		Path:                              ptr.String(path.YPath().String()),
		TransactionalOptions:              convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:               nil, // todo
		MasterReadOptions:                 convertMasterReadOptions(opts.MasterReadOptions),
		SuppressableAccessTrackingOptions: convertAccessTrackingOptions(opts.AccessTrackingOptions),
	}

	call := e.newCall(MethodNodeExists, NewNodeExistsRequest(req), nil)

	var rsp rpc_proxy.TRspExistsNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	ok = rsp.GetExists()
	return
}

func (e *Encoder) RemoveNode(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.RemoveNodeOptions,
) (err error) {
	if opts == nil {
		opts = &yt.RemoveNodeOptions{}
	}

	req := &rpc_proxy.TReqRemoveNode{
		Path:                 ptr.String(path.YPath().String()),
		Recursive:            &opts.Recursive,
		Force:                &opts.Force,
		TransactionalOptions: convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:  convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodRemoveNode, NewRemoveNodeRequest(req), nil)

	var rsp rpc_proxy.TRspRemoveNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) GetNode(
	ctx context.Context,
	path ypath.YPath,
	result interface{},
	opts *yt.GetNodeOptions,
) (err error) {
	if opts == nil {
		opts = &yt.GetNodeOptions{}
	}

	req := &rpc_proxy.TReqGetNode{
		Path:                              ptr.String(path.YPath().String()),
		Attributes:                        convertAttributeKeys(opts.Attributes),
		MaxSize:                           opts.MaxSize,
		TransactionalOptions:              convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:               convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MasterReadOptions:                 convertMasterReadOptions(opts.MasterReadOptions),
		SuppressableAccessTrackingOptions: convertAccessTrackingOptions(opts.AccessTrackingOptions),
	}

	call := e.newCall(MethodGetNode, NewGetNodeRequest(req), nil)

	var rsp rpc_proxy.TRspGetNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return err
	}

	if err := yson.Unmarshal(rsp.Value, result); err != nil {
		return err
	}

	return nil
}

func (e *Encoder) SetNode(
	ctx context.Context,
	path ypath.YPath,
	value interface{},
	opts *yt.SetNodeOptions,
) (err error) {
	if opts == nil {
		opts = &yt.SetNodeOptions{}
	}

	valueBytes, err := yson.Marshal(value)
	if err != nil {
		err = xerrors.Errorf("unable to serialize value: %w", err)
		return
	}

	req := &rpc_proxy.TReqSetNode{
		Path:                              ptr.String(path.YPath().String()),
		Value:                             valueBytes,
		Recursive:                         &opts.Recursive,
		Force:                             &opts.Force,
		TransactionalOptions:              convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:               convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:                   convertMutatingOptions(opts.MutatingOptions),
		SuppressableAccessTrackingOptions: convertAccessTrackingOptions(opts.AccessTrackingOptions),
	}

	call := e.newCall(MethodSetNode, NewSetNodeRequest(req), nil)

	var rsp rpc_proxy.TRspSetNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return err
	}

	return nil
}

func (e *Encoder) MultisetAttributes(
	ctx context.Context,
	path ypath.YPath,
	attrs map[string]interface{},
	opts *yt.MultisetAttributesOptions,
) (err error) {
	if opts == nil {
		opts = &yt.MultisetAttributesOptions{}
	}

	subrequests := make([]*rpc_proxy.TReqMultisetAttributesNode_TSubrequest, 0, len(attrs))
	for key, value := range attrs {
		valueBytes, err := yson.Marshal(value)
		if err != nil {
			return xerrors.Errorf("unable to serialize attribute %q: %w", value, err)
		}

		subrequests = append(subrequests, &rpc_proxy.TReqMultisetAttributesNode_TSubrequest{
			Attribute: ptr.String(key),
			Value:     valueBytes,
		})
	}

	req := &rpc_proxy.TReqMultisetAttributesNode{
		Path:                              ptr.String(path.YPath().String()),
		Subrequests:                       subrequests,
		TransactionalOptions:              convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:               convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:                   convertMutatingOptions(opts.MutatingOptions),
		SuppressableAccessTrackingOptions: convertAccessTrackingOptions(opts.AccessTrackingOptions),
	}

	call := e.newCall(MethodMultisetAttributesNode, NewMultisetAttributesRequest(req), nil)

	var rsp rpc_proxy.TRspMultisetAttributesNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return err
	}

	return nil
}

func (e *Encoder) ListNode(
	ctx context.Context,
	path ypath.YPath,
	result interface{},
	opts *yt.ListNodeOptions,
) (err error) {
	if opts == nil {
		opts = &yt.ListNodeOptions{}
	}

	req := &rpc_proxy.TReqListNode{
		Path:                              ptr.String(path.YPath().String()),
		Attributes:                        convertAttributeKeys(opts.Attributes),
		MaxSize:                           opts.MaxSize,
		TransactionalOptions:              convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:               convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MasterReadOptions:                 convertMasterReadOptions(opts.MasterReadOptions),
		SuppressableAccessTrackingOptions: convertAccessTrackingOptions(opts.AccessTrackingOptions),
	}

	call := e.newCall(MethodListNode, NewListNodeRequest(req), nil)

	var rsp rpc_proxy.TRspListNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return err
	}

	if err := yson.Unmarshal(rsp.Value, result); err != nil {
		return err
	}

	return nil
}

func (e *Encoder) CopyNode(
	ctx context.Context,
	src ypath.YPath,
	dst ypath.YPath,
	opts *yt.CopyNodeOptions,
) (id yt.NodeID, err error) {
	if opts == nil {
		opts = &yt.CopyNodeOptions{}
	}

	req := &rpc_proxy.TReqCopyNode{
		SrcPath:                   ptr.String(src.YPath().String()),
		DstPath:                   ptr.String(dst.YPath().String()),
		Recursive:                 &opts.Recursive,
		Force:                     &opts.Force,
		PreserveAccount:           opts.PreserveAccount,
		PreserveCreationTime:      opts.PreserveCreationTime,
		PreserveModificationTime:  nil, // todo
		PreserveExpirationTime:    opts.PreserveExpirationTime,
		PreserveExpirationTimeout: opts.PreserveExpirationTimeout,
		PreserveOwner:             nil, // todo
		PreserveAcl:               nil, // todo
		IgnoreExisting:            &opts.IgnoreExisting,
		LockExisting:              nil, // todo
		PessimisticQuotaCheck:     opts.PessimisticQuotaCheck,
		TransactionalOptions:      convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:       convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:           convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodCopyNode, NewCopyNodeRequest(req), nil)

	var rsp rpc_proxy.TRspCopyNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	id = makeNodeID(rsp.GetNodeId())
	return
}

func (e *Encoder) MoveNode(
	ctx context.Context,
	src ypath.YPath,
	dst ypath.YPath,
	opts *yt.MoveNodeOptions,
) (id yt.NodeID, err error) {
	if opts == nil {
		opts = &yt.MoveNodeOptions{}
	}

	req := &rpc_proxy.TReqMoveNode{
		SrcPath:                   ptr.String(src.YPath().String()),
		DstPath:                   ptr.String(dst.YPath().String()),
		Recursive:                 &opts.Recursive,
		Force:                     &opts.Force,
		PreserveAccount:           opts.PreserveAccount,
		PreserveCreationTime:      nil, // todo
		PreserveModificationTime:  nil, // todo
		PreserveExpirationTime:    opts.PreserveExpirationTime,
		PreserveExpirationTimeout: opts.PreserveExpirationTimeout,
		PreserveOwner:             nil, // todo
		PessimisticQuotaCheck:     opts.PessimisticQuotaCheck,
		TransactionalOptions:      convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:       convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:           convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodMoveNode, NewMoveNodeRequest(req), nil)

	var rsp rpc_proxy.TRspMoveNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	id = makeNodeID(rsp.GetNodeId())
	return
}

func (e *Encoder) LinkNode(
	ctx context.Context,
	target ypath.YPath,
	link ypath.YPath,
	opts *yt.LinkNodeOptions,
) (id yt.NodeID, err error) {
	if opts == nil {
		opts = &yt.LinkNodeOptions{}
	}

	req := &rpc_proxy.TReqLinkNode{
		SrcPath:              ptr.String(target.YPath().String()),
		DstPath:              ptr.String(link.YPath().String()),
		Recursive:            &opts.Recursive,
		Force:                &opts.Force,
		IgnoreExisting:       &opts.IgnoreExisting,
		LockExisting:         nil, // todo
		TransactionalOptions: convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:  convertPrerequisiteOptions(opts.PrerequisiteOptions),
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodLinkNode, NewLinkNodeRequest(req), nil)

	var rsp rpc_proxy.TRspMoveNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	id = makeNodeID(rsp.GetNodeId())
	return
}

var _ yt.FileClient = (*client)(nil)

func (e *Encoder) WriteFile(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.WriteFileOptions,
) (w io.WriteCloser, err error) {
	return nil, xerrors.New("implement me")
}

func (e *Encoder) ReadFile(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.ReadFileOptions,
) (r io.ReadCloser, err error) {
	return nil, xerrors.New("implement me")
}

func (e *Encoder) PutFileToCache(
	ctx context.Context,
	path ypath.YPath,
	md5 string,
	opts *yt.PutFileToCacheOptions,
) (cachedPath ypath.YPath, err error) {
	return nil, xerrors.New("implement me")
}

func (e *Encoder) GetFileFromCache(
	ctx context.Context,
	md5 string,
	opts *yt.GetFileFromCacheOptions,
) (path ypath.YPath, err error) {
	return ypath.Path(""), xerrors.New("implement me")
}

var _ yt.TableClient = (*client)(nil)

func (e *Encoder) WriteTable(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.WriteTableOptions,
) (w yt.TableWriter, err error) {
	return nil, xerrors.New("implement me")
}

func (e *Encoder) ReadTable(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.ReadTableOptions,
) (r yt.TableReader, err error) {
	return nil, xerrors.New("implement me")
}

var _ yt.TabletClient = (*client)(nil)

func (e *Encoder) SelectRows(
	ctx context.Context,
	query string,
	opts *yt.SelectRowsOptions,
) (r yt.TableReader, err error) {
	if opts == nil {
		opts = &yt.SelectRowsOptions{}
	}

	req := &rpc_proxy.TReqSelectRows{
		Query:                             ptr.String(query),
		Timestamp:                         convertTimestamp(opts.Timestamp),
		InputRowLimit:                     intPtrToUint64Ptr(opts.InputRowLimit),
		OutputRowLimit:                    intPtrToUint64Ptr(opts.OutputRowLimit),
		RangeExpansionLimit:               nil, // todo
		FailOnIncompleteResult:            opts.FailOnIncompleteResult,
		VerboseLogging:                    nil, // todo
		EnableCodeCache:                   nil, // todo
		MaxSubqueries:                     nil, // todo
		AllowFullScan:                     nil, // todo
		AllowJoinWithoutIndex:             nil, // todo
		UdfRegistryPath:                   nil, // todo
		MemoryLimitPerNode:                nil, // todo
		ExecutionPool:                     nil, // todo
		SuppressableAccessTrackingOptions: nil, // todo
	}

	call := e.newCall(MethodSelectRows, NewSelectRowsRequest(req), nil)

	var rsp rpc_proxy.TRspSelectRows
	return e.InvokeReadRow(ctx, call, &rsp)
}

func (e *Encoder) LookupRows(
	ctx context.Context,
	path ypath.Path,
	keys []interface{},
	opts *yt.LookupRowsOptions,
) (r yt.TableReader, err error) {
	if opts == nil {
		opts = &yt.LookupRowsOptions{}
	}

	attachments, descriptor, err := encodeToWire(keys)
	if err != nil {
		err = xerrors.Errorf("unable to encode request into wire format: %w", err)
		return
	}

	req := &rpc_proxy.TReqLookupRows{
		Path:                ptr.String(path.String()),
		Timestamp:           convertTimestamp(opts.Timestamp),
		Columns:             opts.Columns,
		KeepMissingRows:     &opts.KeepMissingRows,
		EnablePartialResult: nil, // todo
		UseLookupCache:      nil, // todo
		TabletReadOptions:   nil, // todo
		MultiplexingBand:    nil, // todo
		RowsetDescriptor:    descriptor,
	}

	call := e.newCall(MethodLookupRows, NewLookupRowsRequest(req), attachments)
	var rsp rpc_proxy.TRspLookupRows
	return e.InvokeReadRow(ctx, call, &rsp)
}

func (e *Encoder) LockRows(
	ctx context.Context,
	path ypath.Path,
	locks []string,
	lockType yt.LockType,
	keys []interface{},
	opts *yt.LockRowsOptions,
) (err error) {
	if len(keys) == 0 {
		return nil
	}

	// todo convert (lock group + type) for api_service.cpp, need table mount cache for that?
	return xerrors.New("implement me")
}

func (e *Encoder) InsertRowBatch(
	ctx context.Context,
	path ypath.Path,
	batch yt.RowBatch,
	opts *yt.InsertRowsOptions,
) (err error) {
	b := batch.(*rowBatch)
	if b.rowCount == 0 {
		return nil
	}

	if opts == nil {
		opts = &yt.InsertRowsOptions{}
	}

	modificationTypes := make([]rpc_proxy.ERowModificationType, 0, b.rowCount)
	for i := 0; i < b.rowCount; i++ {
		modificationTypes = append(modificationTypes, rpc_proxy.ERowModificationType_RMT_WRITE)
	}

	req := &rpc_proxy.TReqModifyRows{
		SequenceNumber:       nil, // todo
		TransactionId:        getTxID(opts.TransactionOptions),
		Path:                 ptr.String(path.String()),
		RowModificationTypes: modificationTypes,
		RowReadLocks:         nil, // todo
		RowLocks:             nil, // todo
		RequireSyncReplica:   opts.RequireSyncReplica,
		UpstreamReplicaId:    nil, // todo
		RowsetDescriptor:     b.descriptor,
	}

	call := e.newCall(MethodModifyRows, NewInsertRowsRequest(req), b.attachments)
	var rsp rpc_proxy.TRspModifyRows
	err = e.Invoke(ctx, call, &rsp)

	return
}

func (e *Encoder) InsertRows(
	ctx context.Context,
	path ypath.Path,
	rows []interface{},
	opts *yt.InsertRowsOptions,
) (err error) {
	batch, err := buildBatch(rows)
	if err != nil {
		return err
	}

	return e.InsertRowBatch(ctx, path, batch, opts)
}

func (e *Encoder) DeleteRows(
	ctx context.Context,
	path ypath.Path,
	keys []interface{},
	opts *yt.DeleteRowsOptions,
) (err error) {
	if opts == nil {
		opts = &yt.DeleteRowsOptions{}
	}

	if len(keys) == 0 {
		return nil
	}

	attachments, descriptor, err := encodeToWire(keys)
	if err != nil {
		err = xerrors.Errorf("unable to encode request into wire format: %w", err)
		return
	}

	modificationTypes := make([]rpc_proxy.ERowModificationType, 0, len(keys))
	for i := 0; i < len(keys); i++ {
		modificationTypes = append(modificationTypes, rpc_proxy.ERowModificationType_RMT_DELETE)
	}

	req := &rpc_proxy.TReqModifyRows{
		SequenceNumber:       nil, // todo
		TransactionId:        getTxID(opts.TransactionOptions),
		Path:                 ptr.String(path.String()),
		RowModificationTypes: modificationTypes,
		RowReadLocks:         nil, // todo
		RowLocks:             nil, // todo
		RequireSyncReplica:   opts.RequireSyncReplica,
		UpstreamReplicaId:    nil, // todo
		RowsetDescriptor:     descriptor,
	}

	call := e.newCall(MethodModifyRows, NewDeleteRowsRequest(req), attachments)
	var rsp rpc_proxy.TRspModifyRows
	err = e.Invoke(ctx, call, &rsp)

	return
}

var _ yt.MountClient = (*client)(nil)

func (e *Encoder) MountTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.MountTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.MountTableOptions{}
	}

	req := &rpc_proxy.TReqMountTable{
		Path:               ptr.String(path.YPath().String()),
		CellId:             convertGUIDPtr(opts.CellID),
		Freeze:             &opts.Freeze,
		MutatingOptions:    convertMutatingOptions(opts.MutatingOptions),
		TabletRangeOptions: convertTabletRangelOptions(opts.TabletRangeOptions),
		TargetCellIds:      convertGUIDs(opts.TargetCellIDs),
	}

	call := e.newCall(MethodMountTable, NewMountTableRequest(req), nil)

	var rsp rpc_proxy.TRspMountTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) UnmountTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.UnmountTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.UnmountTableOptions{}
	}

	req := &rpc_proxy.TReqUnmountTable{
		Path:               ptr.String(path.YPath().String()),
		Force:              &opts.Force,
		MutatingOptions:    convertMutatingOptions(opts.MutatingOptions),
		TabletRangeOptions: convertTabletRangelOptions(opts.TabletRangeOptions),
	}

	call := e.newCall(MethodUnmountTable, NewUnmountTableRequest(req), nil)

	var rsp rpc_proxy.TRspUnmountTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) RemountTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.RemountTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.RemountTableOptions{}
	}

	req := &rpc_proxy.TReqRemountTable{
		Path:               ptr.String(path.YPath().String()),
		MutatingOptions:    convertMutatingOptions(opts.MutatingOptions),
		TabletRangeOptions: convertTabletRangelOptions(opts.TabletRangeOptions),
	}

	call := e.newCall(MethodRemountTable, NewRemountTableRequest(req), nil)

	var rsp rpc_proxy.TRspRemountTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) ReshardTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.ReshardTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.ReshardTableOptions{}
	}

	attachments, err := encodePivotKeys(opts.PivotKeys)
	if err != nil {
		return xerrors.Errorf("unable to encode pivot keys: %w", err)
	}

	req := &rpc_proxy.TReqReshardTable{
		Path:               ptr.String(path.YPath().String()),
		TabletCount:        intPtrToInt32Ptr(opts.TabletCount),
		Uniform:            nil, // todo
		MutatingOptions:    convertMutatingOptions(opts.MutatingOptions),
		TabletRangeOptions: convertTabletRangelOptions(opts.TabletRangeOptions),
		RowsetDescriptor:   nil,
	}

	call := e.newCall(MethodResshardTable, NewReshardTableRequest(req), attachments)
	var rsp rpc_proxy.TRspReshardTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) AlterTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.AlterTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.AlterTableOptions{}
	}

	schemaBytes, err := yson.Marshal(opts.Schema)
	if err != nil {
		return xerrors.Errorf("unable to serialize schema: %w", err)
	}

	req := &rpc_proxy.TReqAlterTable{
		Path:                 ptr.String(path.YPath().String()),
		Schema:               schemaBytes,
		Dynamic:              opts.Dynamic,
		UpstreamReplicaId:    convertGUIDPtr(opts.UpstreamReplicaID),
		SchemaModification:   nil, // todo
		TransactionalOptions: nil, // todo
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodAlterTable, NewAlterTableRequest(req), nil)

	var rsp rpc_proxy.TRspAlterTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) FreezeTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.FreezeTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.FreezeTableOptions{}
	}

	req := &rpc_proxy.TReqFreezeTable{
		Path:               ptr.String(path.YPath().String()),
		MutatingOptions:    convertMutatingOptions(opts.MutatingOptions),
		TabletRangeOptions: convertTabletRangelOptions(opts.TabletRangeOptions),
	}

	call := e.newCall(MethodFreezeTable, NewFreezeTableRequest(req), nil)

	var rsp rpc_proxy.TRspFreezeTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) UnfreezeTable(
	ctx context.Context,
	path ypath.Path,
	opts *yt.UnfreezeTableOptions,
) (err error) {
	if opts == nil {
		opts = &yt.UnfreezeTableOptions{}
	}

	req := &rpc_proxy.TReqUnfreezeTable{
		Path:               ptr.String(path.YPath().String()),
		MutatingOptions:    convertMutatingOptions(opts.MutatingOptions),
		TabletRangeOptions: convertTabletRangelOptions(opts.TabletRangeOptions),
	}

	call := e.newCall(MethodUnfreezeTable, NewUnfreezeTableRequest(req), nil)

	var rsp rpc_proxy.TRspUnfreezeTable
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) AlterTableReplica(
	ctx context.Context,
	id yt.NodeID,
	opts *yt.AlterTableReplicaOptions,
) (err error) {
	if opts == nil {
		opts = &yt.AlterTableReplicaOptions{}
	}

	tableReplicaMode, err := convertTableReplicaMode(opts.Mode)
	if err != nil {
		return err
	}

	req := &rpc_proxy.TReqAlterTableReplica{
		ReplicaId:          convertGUID(guid.GUID(id)),
		Enabled:            opts.Enabled,
		Mode:               tableReplicaMode,
		PreserveTimestamps: nil, // todo
		Atomicity:          nil, // todo
	}

	call := e.newCall(MethodAlterTable, NewAlterTableReplicaRequest(req), nil)

	var rsp rpc_proxy.TRspAlterTableReplica
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) StartTx(
	ctx context.Context,
	opts *yt.StartTxOptions,
) (id yt.TxID, err error) {
	id, _, err = e.startTx(ctx, opts)

	return
}

func (e *Encoder) startTx(
	ctx context.Context,
	opts *yt.StartTxOptions,
) (id yt.TxID, ts yt.Timestamp, err error) {
	if opts == nil {
		opts = &yt.StartTxOptions{}
	}
	if opts.TransactionOptions == nil {
		opts.TransactionOptions = &yt.TransactionOptions{}
	}

	attrs, err := convertAttributes(opts.Attributes)
	if err != nil {
		err = xerrors.Errorf("unable to serialize attributes: %w", err)
		return
	}

	txType := rpc_proxy.ETransactionType_TT_MASTER
	req := &rpc_proxy.TReqStartTransaction{
		Type:                       &txType,
		Timeout:                    convertDuration(opts.Timeout),
		Id:                         nil, // todo
		ParentId:                   convertParentTxID(opts.TransactionID),
		AutoAbort:                  nil, // todo
		Sticky:                     &opts.Sticky,
		Ping:                       &opts.Ping,
		PingAncestors:              &opts.PingAncestors,
		Atomicity:                  nil, // todo
		Durability:                 nil, // todo
		Attributes:                 attrs,
		Deadline:                   convertTime(opts.Deadline),
		PrerequisiteTransactionIds: convertPrerequisiteTxIDs(opts.PrerequisiteTransactionIDs),
	}

	call := e.newCall(MethodStartTransaction, NewStartTxRequest(req), nil)

	var rsp rpc_proxy.TRspStartTransaction
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	id = yt.TxID(makeNodeID(rsp.GetId()))
	ts = yt.Timestamp(rsp.GetStartTimestamp())

	return
}

func (e *Encoder) StartTabletTx(
	ctx context.Context,
	opts *yt.StartTabletTxOptions,
) (id yt.TxID, err error) {
	id, _, err = e.startTabletTx(ctx, opts)

	return
}

func (e *Encoder) startTabletTx(
	ctx context.Context,
	opts *yt.StartTabletTxOptions,
) (id yt.TxID, ts yt.Timestamp, err error) {
	if opts == nil {
		opts = &yt.StartTabletTxOptions{
			Sticky: true,
		}
	}

	atomicity, err := convertAtomicity(opts.Atomicity)
	if err != nil {
		return
	}

	txType := rpc_proxy.ETransactionType_TT_TABLET
	req := &rpc_proxy.TReqStartTransaction{
		Type:                       &txType,
		Timeout:                    convertDuration(opts.Timeout),
		Id:                         nil, // todo
		ParentId:                   nil, // todo
		AutoAbort:                  nil, // todo
		Sticky:                     &opts.Sticky,
		Ping:                       nil, // todo
		PingAncestors:              nil, // todo
		Atomicity:                  atomicity,
		Durability:                 nil, // todo
		Attributes:                 nil, // todo
		Deadline:                   nil, // todo
		PrerequisiteTransactionIds: nil, // todo
	}

	call := e.newCall(MethodStartTransaction, NewStartTxRequest(req), nil)

	var rsp rpc_proxy.TRspStartTransaction
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	id = yt.TxID(makeNodeID(rsp.GetId()))
	ts = yt.Timestamp(rsp.GetStartTimestamp())

	return
}

func (e *Encoder) PingTx(
	ctx context.Context,
	id yt.TxID,
	opts *yt.PingTxOptions,
) (err error) {
	if opts == nil {
		opts = &yt.PingTxOptions{}
	}
	if opts.TransactionOptions == nil {
		opts.TransactionOptions = &yt.TransactionOptions{}
	}

	req := &rpc_proxy.TReqPingTransaction{
		TransactionId: convertTxID(id),
		PingAncestors: &opts.PingAncestors,
	}

	call := e.newCall(MethodPingTransaction, NewPingTxRequest(req), nil)

	var rsp rpc_proxy.TRspPingTransaction
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) AbortTx(
	ctx context.Context,
	id yt.TxID,
	opts *yt.AbortTxOptions,
) (err error) {
	req := &rpc_proxy.TReqAbortTransaction{
		TransactionId: convertTxID(id),
	}

	call := e.newCall(MethodAbortTransaction, NewAbortTxRequest(req), nil)

	var rsp rpc_proxy.TRspAbortTransaction
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) CommitTx(
	ctx context.Context,
	id yt.TxID,
	opts *yt.CommitTxOptions,
) (err error) {
	if opts == nil {
		opts = &yt.CommitTxOptions{}
	}

	req := &rpc_proxy.TReqCommitTransaction{
		TransactionId:       convertTxID(id),
		PrerequisiteOptions: convertPrerequisiteOptions(opts.PrerequisiteOptions),
	}

	call := e.newCall(MethodCommitTransaction, NewCommitTxRequest(req), nil)

	var rsp rpc_proxy.TRspCommitTransaction
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) AddMember(
	ctx context.Context,
	group string,
	member string,
	opts *yt.AddMemberOptions,
) (err error) {
	if opts == nil {
		opts = &yt.AddMemberOptions{}
	}

	req := &rpc_proxy.TReqAddMember{
		Group:               &group,
		Member:              &member,
		MutatingOptions:     convertMutatingOptions(opts.MutatingOptions),
		PrerequisiteOptions: convertPrerequisiteOptions(opts.PrerequisiteOptions),
	}

	call := e.newCall(MethodAddMember, NewAddMemberRequest(req), nil)

	var rsp rpc_proxy.TRspAddMember
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) RemoveMember(
	ctx context.Context,
	group string,
	member string,
	opts *yt.RemoveMemberOptions,
) (err error) {
	if opts == nil {
		opts = &yt.RemoveMemberOptions{}
	}

	req := &rpc_proxy.TReqRemoveMember{
		Group:               &group,
		Member:              &member,
		MutatingOptions:     convertMutatingOptions(opts.MutatingOptions),
		PrerequisiteOptions: convertPrerequisiteOptions(opts.PrerequisiteOptions),
	}

	call := e.newCall(MethodRemoveMember, NewRemoveMemberRequest(req), nil)

	var rsp rpc_proxy.TRspRemoveMember
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) StartOperation(
	ctx context.Context,
	opType yt.OperationType,
	spec interface{},
	opts *yt.StartOperationOptions,
) (opID yt.OperationID, err error) {
	if opts == nil {
		opts = &yt.StartOperationOptions{}
	}

	operationType, err := convertOperationType(&opType)
	if err != nil {
		return
	}

	specBytes, err := yson.Marshal(spec)
	if err != nil {
		err = xerrors.Errorf("unable to serialize spec: %w", err)
		return
	}

	req := &rpc_proxy.TReqStartOperation{
		Type:                 operationType,
		Spec:                 specBytes,
		TransactionalOptions: convertTransactionOptions(opts.TransactionOptions),
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodStartOperation, NewStartOperationRequest(req), nil)

	var rsp rpc_proxy.TRspStartOperation
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	opID = yt.OperationID(makeNodeID(rsp.GetOperationId()))
	return
}

func (e *Encoder) AbortOperation(
	ctx context.Context,
	opID yt.OperationID,
	opts *yt.AbortOperationOptions,
) (err error) {
	if opts == nil {
		opts = &yt.AbortOperationOptions{}
	}

	req := &rpc_proxy.TReqAbortOperation{
		OperationIdOrAlias: &rpc_proxy.TReqAbortOperation_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
		AbortMessage: opts.AbortMessage,
	}

	call := e.newCall(MethodAbortOperation, NewAbortOperationRequest(req), nil)

	var rsp rpc_proxy.TRspAbortOperation
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) SuspendOperation(
	ctx context.Context,
	opID yt.OperationID,
	opts *yt.SuspendOperationOptions,
) (err error) {
	if opts == nil {
		opts = &yt.SuspendOperationOptions{}
	}

	req := &rpc_proxy.TReqSuspendOperation{
		OperationIdOrAlias: &rpc_proxy.TReqSuspendOperation_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
		AbortRunningJobs: &opts.AbortRunningJobs,
	}

	call := e.newCall(MethodSuspendOperation, NewSuspendOperationRequest(req), nil)

	var rsp rpc_proxy.TRspSuspendOperation
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) ResumeOperation(
	ctx context.Context,
	opID yt.OperationID,
	opts *yt.ResumeOperationOptions,
) (err error) {
	req := &rpc_proxy.TReqResumeOperation{
		OperationIdOrAlias: &rpc_proxy.TReqResumeOperation_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
	}

	call := e.newCall(MethodResumeOperation, NewResumeOperationRequest(req), nil)

	var rsp rpc_proxy.TRspResumeOperation
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) CompleteOperation(
	ctx context.Context,
	opID yt.OperationID,
	opts *yt.CompleteOperationOptions,
) (err error) {
	req := &rpc_proxy.TReqCompleteOperation{
		OperationIdOrAlias: &rpc_proxy.TReqCompleteOperation_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
	}

	call := e.newCall(MethodCompleteOperation, NewCompleteOperationRequest(req), nil)

	var rsp rpc_proxy.TRspCompleteOperation
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) UpdateOperationParameters(
	ctx context.Context,
	opID yt.OperationID,
	params interface{},
	opts *yt.UpdateOperationParametersOptions,
) (err error) {
	paramsBytes, err := yson.Marshal(params)
	if err != nil {
		err = xerrors.Errorf("unable to serialize params: %w", err)
		return
	}

	req := &rpc_proxy.TReqUpdateOperationParameters{
		OperationIdOrAlias: &rpc_proxy.TReqUpdateOperationParameters_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
		Parameters: paramsBytes,
	}

	call := e.newCall(MethodUpdateOperationParameters, NewUpdateOperationParametersRequest(req), nil)

	var rsp rpc_proxy.TRspUpdateOperationParameters
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) GetOperation(
	ctx context.Context,
	opID yt.OperationID,
	opts *yt.GetOperationOptions,
) (status *yt.OperationStatus, err error) {
	if opts == nil {
		opts = &yt.GetOperationOptions{}
	}

	req := &rpc_proxy.TReqGetOperation{
		OperationIdOrAlias: &rpc_proxy.TReqGetOperation_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
		Attributes:                opts.Attributes,
		IncludeRuntime:            opts.IncludeRuntime,
		MaximumCypressProgressAge: nil, // todo
		MasterReadOptions:         convertMasterReadOptions(opts.MasterReadOptions),
	}

	call := e.newCall(MethodGetOperation, NewGetOperationRequest(req), nil)

	var rsp rpc_proxy.TRspGetOperation
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	status = &yt.OperationStatus{}
	if err := yson.Unmarshal(rsp.Meta, status); err != nil {
		return nil, err
	}

	return
}

func (e *Encoder) ListOperations(
	ctx context.Context,
	opts *yt.ListOperationsOptions,
) (operations *yt.ListOperationsResult, err error) {
	if opts == nil {
		opts = &yt.ListOperationsOptions{}
	}

	opState, err := convertOperationState(opts.State)
	if err != nil {
		return nil, err
	}

	opType, err := convertOperationType(opts.Type)
	if err != nil {
		return nil, err
	}

	var limit *uint64
	if opts.Limit != nil {
		limit = ptr.Uint64(uint64(*opts.Limit))
	}

	req := &rpc_proxy.TReqListOperations{
		FromTime:               convertTime(opts.FromTime),
		ToTime:                 convertTime(opts.ToTime),
		CursorTime:             convertTime(opts.Cursor),
		CursorDirection:        nil, // todo
		UserFilter:             opts.User,
		StateFilter:            opState,
		TypeFilter:             opType,
		SubstrFilter:           opts.Filter,
		Pool:                   nil,   // todo
		IncludeArchive:         nil,   // todo
		IncludeCounters:        nil,   // todo
		Limit:                  limit, // todo
		Attributes:             nil,   // todo
		AccessFilter:           nil,   // todo
		ArchiveFetchingTimeout: nil,   // todo
		MasterReadOptions:      convertMasterReadOptions(opts.MasterReadOptions),
	}

	call := e.newCall(MethodListOperations, NewListOperationsRequest(req), nil)

	var rsp rpc_proxy.TRspListOperations
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	operations, err = makeListOperationsResult(rsp.Result)
	if err != nil {
		return nil, xerrors.Errorf("unable to deserializer response: %w", err)
	}

	return
}

func (e *Encoder) ListJobs(
	ctx context.Context,
	opID yt.OperationID,
	opts *yt.ListJobsOptions,
) (r *yt.ListJobsResult, err error) {
	if opts == nil {
		opts = &yt.ListJobsOptions{}
	}

	jobType, err := convertJobType(opts.JobType)
	if err != nil {
		return nil, err
	}

	jobState, err := convertJobState(opts.JobState)
	if err != nil {
		return nil, err
	}

	sortOrder, err := convertJobSortOrder(opts.SortOrder)
	if err != nil {
		return nil, err
	}

	var limit *int64
	if opts.Limit != nil {
		limit = ptr.Int64(int64(*opts.Limit))
	}

	var offset *int64
	if opts.Offset != nil {
		offset = ptr.Int64(int64(*opts.Offset))
	}

	dataSource, err := convertDataSource(opts.DataSource)
	if err != nil {
		return nil, err
	}

	req := &rpc_proxy.TReqListJobs{
		OperationIdOrAlias: &rpc_proxy.TReqListJobs_OperationId{
			OperationId: convertGUID(guid.GUID(opID)),
		},
		Type:                        jobType,
		State:                       jobState,
		Address:                     opts.Address,
		WithStderr:                  opts.WithStderr,
		WithFailContext:             opts.WithFailContext,
		WithSpec:                    nil, // todo
		SortField:                   nil, // todo
		SortOrder:                   sortOrder,
		Limit:                       limit,
		Offset:                      offset,
		IncludeCypress:              nil, // todo
		IncludeControllerAgent:      nil, // todo
		IncludeArchive:              nil, // todo
		DataSource:                  dataSource,
		RunningJobsLookbehindPeriod: nil, // todo
		JobCompetitionId:            nil, // todo
		WithCompetitors:             nil, // todo
		TaskName:                    nil, // todo
		MasterReadOptions:           nil, // todo
	}

	call := e.newCall(MethodListJobs, NewListJobsRequest(req), nil)

	var rsp rpc_proxy.TRspListJobs
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	r, err = makeListJobsResult(rsp.Result)
	if err != nil {
		return nil, xerrors.Errorf("unable to deserializer response: %w", err)
	}

	return
}

func (e *Encoder) GetJobStderr(
	ctx context.Context,
	opID yt.OperationID,
	jobID yt.JobID,
	opts *yt.GetJobStderrOptions,
) (r []byte, err error) {
	return nil, xerrors.New("implement me")
}

func (e *Encoder) LockNode(
	ctx context.Context,
	path ypath.YPath,
	mode yt.LockMode,
	opts *yt.LockNodeOptions,
) (lr yt.LockResult, err error) {
	if opts == nil {
		opts = &yt.LockNodeOptions{}
	}

	lockMode, err := convertLockMode(mode)
	if err != nil {
		return yt.LockResult{}, err
	}

	req := &rpc_proxy.TReqLockNode{
		Path:                 ptr.String(path.YPath().String()),
		Mode:                 ptr.Int32(int32(lockMode)),
		Waitable:             &opts.Waitable,
		ChildKey:             opts.ChildKey,
		AttributeKey:         opts.AttributeKey,
		TransactionalOptions: convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:  nil, // todo
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodLockNode, NewLockNodeRequest(req), nil)

	var rsp rpc_proxy.TRspLockNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	lr, err = makeLockNodeResult(&rsp)
	if err != nil {
		return yt.LockResult{}, xerrors.Errorf("unable to deserializer response: %w", err)
	}

	return
}

func (e *Encoder) UnlockNode(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.UnlockNodeOptions,
) (err error) {
	if opts == nil {
		opts = &yt.UnlockNodeOptions{}
	}

	req := &rpc_proxy.TReqUnlockNode{
		Path:                 ptr.String(path.YPath().String()),
		TransactionalOptions: convertTransactionOptions(opts.TransactionOptions),
		PrerequisiteOptions:  nil, // todo
		MutatingOptions:      convertMutatingOptions(opts.MutatingOptions),
	}

	call := e.newCall(MethodUnlockNode, NewUnlockNodeRequest(req), nil)

	var rsp rpc_proxy.TRspUnlockNode
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	return
}

func (e *Encoder) GenerateTimestamp(
	ctx context.Context,
	opts *yt.GenerateTimestampOptions,
) (ts yt.Timestamp, err error) {
	req := &rpc_proxy.TReqGenerateTimestamps{
		Count: nil, // todo
	}

	call := e.newCall(MethodGenerateTimestamps, NewGenerateTimestampRequest(req), nil)

	var rsp rpc_proxy.TRspGenerateTimestamps
	err = e.Invoke(ctx, call, &rsp)
	if err != nil {
		return
	}

	ts = yt.Timestamp(rsp.GetTimestamp())
	return
}

func (e *Encoder) LocateSkynetShare(
	ctx context.Context,
	path ypath.YPath,
	opts *yt.LocateSkynetShareOptions,
) (l yt.ShareLocation, err error) {
	return yt.ShareLocation{}, xerrors.New("implement me")
}

func (e *Encoder) GetInSyncReplicas(
	ctx context.Context,
	path ypath.Path,
	ts yt.Timestamp,
	keys []interface{},
	opts *yt.GetInSyncReplicasOptions,
) (ids []yt.NodeID, err error) {
	return nil, xerrors.New("implement me")
}
