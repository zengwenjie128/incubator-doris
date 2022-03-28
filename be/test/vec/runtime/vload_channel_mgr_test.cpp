// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/runtime/vload_channel_mgr.h"

#include <gtest/gtest.h>

#include "common/object_pool.h"
#include "gen_cpp/Descriptors_types.h"
#include "gen_cpp/PaloInternalService_types.h"
#include "gen_cpp/Types_types.h"
#include "vec/olap/vdelta_writer.h"
#include "olap/memtable_flush_executor.h"
#include "olap/schema.h"
#include "olap/storage_engine.h"
#include "runtime/descriptor_helper.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/mem_tracker.h"
#include "runtime/primitive_type.h"
#include "runtime/row_batch.h"
#include "runtime/tuple_row.h"
#include "util/thrift_util.h"

namespace doris {

std::unordered_map<int64_t, int> _k_tablet_recorder;
OLAPStatus open_status;
OLAPStatus add_status;
OLAPStatus close_status;
int64_t wait_lock_time_ns;

// mock
DeltaWriter::DeltaWriter(WriteRequest* req, const std::shared_ptr<MemTracker>& mem_tracker,
                         StorageEngine* storage_engine)
        : _req(*req) {}

DeltaWriter::~DeltaWriter() {}

OLAPStatus DeltaWriter::init() {
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::open(WriteRequest* req, const std::shared_ptr<MemTracker>& mem_tracker,
                             DeltaWriter** writer) {
    if (open_status != OLAP_SUCCESS) {
        return open_status;
    }
    *writer = new DeltaWriter(req, mem_tracker, nullptr);
    return open_status;
}

OLAPStatus DeltaWriter::write(Tuple* tuple) {
    if (_k_tablet_recorder.find(_req.tablet_id) == std::end(_k_tablet_recorder)) {
        _k_tablet_recorder[_req.tablet_id] = 1;
    } else {
        _k_tablet_recorder[_req.tablet_id]++;
    }
    return add_status;
}

OLAPStatus DeltaWriter::write(const RowBatch* row_batch, const std::vector<int>& row_idxs) {
    if (_k_tablet_recorder.find(_req.tablet_id) == std::end(_k_tablet_recorder)) {
        _k_tablet_recorder[_req.tablet_id] = 0;
    }
    _k_tablet_recorder[_req.tablet_id] += row_idxs.size();
    return add_status;
}

OLAPStatus DeltaWriter::close() {
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::close_wait(google::protobuf::RepeatedPtrField<PTabletInfo>* tablet_vec, bool is_broken) {
    return close_status;
}

OLAPStatus DeltaWriter::cancel() {
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::flush_memtable_and_wait(bool need_wait) {
    return OLAP_SUCCESS;
}

OLAPStatus DeltaWriter::wait_flush() {
    return OLAP_SUCCESS;
}

int64_t DeltaWriter::partition_id() const {
    return 1L;
}
int64_t DeltaWriter::mem_consumption() const {
    return 1024L;
}

namespace vectorized {

VDeltaWriter::VDeltaWriter(WriteRequest* req, const std::shared_ptr<MemTracker>& parent,
                           StorageEngine* storage_engine)
        : DeltaWriter(req, parent, storage_engine) {}

VDeltaWriter::~VDeltaWriter() {}

OLAPStatus VDeltaWriter::open(WriteRequest* req, const std::shared_ptr<MemTracker>& mem_tracker,
                              VDeltaWriter** writer) {
    if (open_status != OLAP_SUCCESS) {
        return open_status;
    }
    *writer = new VDeltaWriter(req, mem_tracker, nullptr);
    return open_status;
}

OLAPStatus VDeltaWriter::write(const Block* block, const std::vector<int>& row_idxs) {
    if (_k_tablet_recorder.find(_req.tablet_id) == std::end(_k_tablet_recorder)) {
        _k_tablet_recorder[_req.tablet_id] = 0;
    }
    _k_tablet_recorder[_req.tablet_id] += row_idxs.size();
    return add_status;
}

}

class VLoadChannelMgrTest : public testing::Test {
public:
    VLoadChannelMgrTest() {}
    virtual ~VLoadChannelMgrTest() {}
    void SetUp() override {
        _k_tablet_recorder.clear();
        open_status = OLAP_SUCCESS;
        add_status = OLAP_SUCCESS;
        close_status = OLAP_SUCCESS;
        config::streaming_load_rpc_max_alive_time_sec = 120;
    }

private:

    size_t uncompressed_size = 0;
    size_t compressed_size = 0;
};

TDescriptorTable create_descriptor_table() {
    TDescriptorTableBuilder dtb;
    TTupleDescriptorBuilder tuple_builder;

    tuple_builder.add_slot(
            TSlotDescriptorBuilder().type(TYPE_INT).column_name("c1").column_pos(0).build());
    tuple_builder.add_slot(
            TSlotDescriptorBuilder().type(TYPE_BIGINT).column_name("c2").column_pos(1).build());
    tuple_builder.build(&dtb);

    return dtb.desc_tbl();
}

Schema create_schema() {
    std::vector<TabletColumn> col_schemas;
    //c1
    TabletColumn c1(OLAP_FIELD_AGGREGATION_NONE, OLAP_FIELD_TYPE_INT, true);
    c1.set_name("c1");

    col_schemas.emplace_back(std::move(c1));
    // c2: int
    TabletColumn c2(OLAP_FIELD_AGGREGATION_NONE, OLAP_FIELD_TYPE_BIGINT, true);
    c2.set_name("c2");
    col_schemas.emplace_back(std::move(c2));

    Schema schema(col_schemas, 2);
    return schema;
}

void create_schema(DescriptorTbl* desc_tbl, POlapTableSchemaParam* pschema) {
    pschema->set_db_id(1);
    pschema->set_table_id(2);
    pschema->set_version(0);

    auto tuple_desc = desc_tbl->get_tuple_descriptor(0);
    tuple_desc->to_protobuf(pschema->mutable_tuple_desc());
    for (auto slot : tuple_desc->slots()) {
        slot->to_protobuf(pschema->add_slot_descs());
    }

    // index schema
    auto indexes = pschema->add_indexes();
    indexes->set_id(4);
    indexes->add_columns("c1");
    indexes->add_columns("c2");
    indexes->set_schema_hash(123);
}

static void create_block(Schema& schema, vectorized::Block& block)
{
    for (auto &column_desc : schema.columns()) {
        ASSERT_TRUE(column_desc);
        auto data_type = Schema::get_data_type_ptr(column_desc->type());
        ASSERT_NE(data_type, nullptr);
        if (column_desc->is_nullable()) {
            data_type = std::make_shared<vectorized::DataTypeNullable>(std::move(data_type));
        }
        auto column = data_type->create_column();
        vectorized::ColumnWithTypeAndName ctn(std::move(column), data_type, column_desc->name());
        block.insert(ctn);
    }
}

TEST_F(VLoadChannelMgrTest, normal) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto schema = create_schema();
    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});
    auto tracker = std::make_shared<MemTracker>();
    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        auto st = mgr.open(request);
        if (!st.ok()) {
            LOG(INFO) << "here we go!!!!";
            LOG(INFO) << st.to_string() << std::endl;
        }
        request.release_id();
        ASSERT_TRUE(st.ok());
    }

    // add a block
    {
        PTabletWriterAddBlockRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_sender_id(0);
        request.set_eos(true);
        request.set_packet_seq(0);

        request.add_tablet_ids(20);
        request.add_tablet_ids(21);
        request.add_tablet_ids(20);

        vectorized::Block block;
        create_block(schema, block);

        auto columns = block.mutate_columns();
        auto& col1 = columns[0];
        auto& col2 = columns[1];

        // row1
        {
            int value = 987654;
            int64_t big_value = 1234567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row2
        {
            int value = 12345678;
            int64_t big_value = 9876567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row3
        {
            int value = 876545678;
            int64_t big_value = 76543234567;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }

        PTabletWriterAddBlockResult response;
        std::string buffer;
        block.serialize(request.mutable_block(),  &uncompressed_size, &compressed_size, &buffer);
        auto st = mgr.add_block(request, &response);
        if (!st.ok()) {
            LOG(INFO) << "here we go!!!!";
            LOG(INFO) << st.to_string() << std::endl;
        }
        request.release_id();
        ASSERT_TRUE(st.ok());
    }
    // check content
    ASSERT_EQ(_k_tablet_recorder[20], 2);
    ASSERT_EQ(_k_tablet_recorder[21], 1);
}

TEST_F(VLoadChannelMgrTest, cancel) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});

    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        auto st = mgr.open(request);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }

    // add a batch
    {
        PTabletWriterCancelRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        auto st = mgr.cancel(request);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }
}

TEST_F(VLoadChannelMgrTest, open_failed) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});

    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        open_status = OLAP_ERR_TABLE_NOT_FOUND;
        auto st = mgr.open(request);
        request.release_id();
        ASSERT_FALSE(st.ok());
    }
}

TEST_F(VLoadChannelMgrTest, add_failed) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto schema = create_schema();
    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});
    auto tracker = std::make_shared<MemTracker>();
    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        auto st = mgr.open(request);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }

    // add a batch
    {
        PTabletWriterAddBlockRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_sender_id(0);
        request.set_eos(true);
        request.set_packet_seq(0);

        request.add_tablet_ids(20);
        request.add_tablet_ids(21);
        request.add_tablet_ids(20);

        vectorized::Block block;
        create_block(schema, block);

        auto columns = block.mutate_columns();
        auto& col1 = columns[0];
        auto& col2 = columns[1];

        // row1
        {
            int value = 987654;
            int64_t big_value = 1234567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row2
        {
            int value = 12345678;
            int64_t big_value = 9876567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row3
        {
            int value = 876545678;
            int64_t big_value = 76543234567;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }

        std::string buffer;
        block.serialize(request.mutable_block(),  &uncompressed_size, &compressed_size, &buffer);
        // DeltaWriter's write will return -215
        add_status = OLAP_ERR_TABLE_NOT_FOUND;
        PTabletWriterAddBlockResult response;
        auto st = mgr.add_block(request, &response);
        request.release_id();
        // st is still ok.
        ASSERT_TRUE(st.ok());
        ASSERT_EQ(2, response.tablet_errors().size());
    }
}


TEST_F(VLoadChannelMgrTest, close_failed) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto schema = create_schema();
    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});
    auto tracker = std::make_shared<MemTracker>();
    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        auto st = mgr.open(request);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }

    // add a batch
    {
        PTabletWriterAddBlockRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_sender_id(0);
        request.set_eos(true);
        request.set_packet_seq(0);

        request.add_tablet_ids(20);
        request.add_tablet_ids(21);
        request.add_tablet_ids(20);

        request.add_partition_ids(10);
        request.add_partition_ids(11);

        vectorized::Block block;
        create_block(schema, block);

        auto columns = block.mutate_columns();
        auto& col1 = columns[0];
        auto& col2 = columns[1];

        // row1
        {
            int value = 987654;
            int64_t big_value = 1234567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row2
        {
            int value = 12345678;
            int64_t big_value = 9876567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row3
        {
            int value = 876545678;
            int64_t big_value = 76543234567;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }

        std::string buffer;
        block.serialize(request.mutable_block(),  &uncompressed_size, &compressed_size, &buffer);
        close_status = OLAP_ERR_TABLE_NOT_FOUND;
        PTabletWriterAddBlockResult response;
        auto st = mgr.add_block(request, &response);
        request.release_id();
        // even if delta close failed, the return status is still ok, but tablet_vec is empty
        ASSERT_TRUE(st.ok());
        ASSERT_TRUE(response.tablet_vec().empty());
    }
}

TEST_F(VLoadChannelMgrTest, unknown_tablet) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto schema = create_schema();
    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});
    auto tracker = std::make_shared<MemTracker>();
    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        auto st = mgr.open(request);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }

    // add a batch
    {
        PTabletWriterAddBlockRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_sender_id(0);
        request.set_eos(true);
        request.set_packet_seq(0);

        request.add_tablet_ids(20);
        request.add_tablet_ids(22);
        request.add_tablet_ids(20);

        vectorized::Block block;
        create_block(schema, block);

        auto columns = block.mutate_columns();
        auto& col1 = columns[0];
        auto& col2 = columns[1];

        // row1
        {
            int value = 987654;
            int64_t big_value = 1234567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row2
        {
            int value = 12345678;
            int64_t big_value = 9876567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row3
        {
            int value = 876545678;
            int64_t big_value = 76543234567;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }

        std::string buffer;
        block.serialize(request.mutable_block(),  &uncompressed_size, &compressed_size, &buffer);
        PTabletWriterAddBlockResult response;
        auto st = mgr.add_block(request, &response);
        request.release_id();
        ASSERT_FALSE(st.ok());
    }
}

TEST_F(VLoadChannelMgrTest, duplicate_packet) {
    ExecEnv env;
    vectorized::VLoadChannelMgr mgr;
    mgr.init(-1);

    auto schema = create_schema();
    auto tdesc_tbl = create_descriptor_table();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    RowDescriptor row_desc(*desc_tbl, {0}, {false});
    auto tracker = std::make_shared<MemTracker>();
    PUniqueId load_id;
    load_id.set_hi(2);
    load_id.set_lo(3);
    {
        PTabletWriterOpenRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_txn_id(1);
        create_schema(desc_tbl, request.mutable_schema());
        for (int i = 0; i < 2; ++i) {
            auto tablet = request.add_tablets();
            tablet->set_partition_id(10 + i);
            tablet->set_tablet_id(20 + i);
        }
        request.set_num_senders(1);
        request.set_need_gen_rollup(false);
        auto st = mgr.open(request);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }

    // add a batch
    {
        PTabletWriterAddBlockRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_sender_id(0);
        request.set_eos(false);
        request.set_packet_seq(0);

        request.add_tablet_ids(20);
        request.add_tablet_ids(21);
        request.add_tablet_ids(20);

        vectorized::Block block;
        create_block(schema, block);

        auto columns = block.mutate_columns();
        auto& col1 = columns[0];
        auto& col2 = columns[1];

        // row1
        {
            int value = 987654;
            int64_t big_value = 1234567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row2
        {
            int value = 12345678;
            int64_t big_value = 9876567899876;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }
        // row3
        {
            int value = 876545678;
            int64_t big_value = 76543234567;
            col1->insert_data((const char*)&value, sizeof(value));
            col2->insert_data((const char*)&big_value, sizeof(big_value));
        }

        std::string buffer;
        block.serialize(request.mutable_block(),  &uncompressed_size, &compressed_size, &buffer);
        PTabletWriterAddBlockResult response;
        auto st = mgr.add_block(request, &response);
        ASSERT_TRUE(st.ok());
        PTabletWriterAddBlockResult response2;
        st = mgr.add_block(request, &response2);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }
    // close
    {
        PTabletWriterAddBlockRequest request;
        request.set_allocated_id(&load_id);
        request.set_index_id(4);
        request.set_sender_id(0);
        request.set_eos(true);
        request.set_packet_seq(0);
        PTabletWriterAddBlockResult response;
        auto st = mgr.add_block(request, &response);
        request.release_id();
        ASSERT_TRUE(st.ok());
    }
    // check content
    ASSERT_EQ(_k_tablet_recorder[20], 2);
    ASSERT_EQ(_k_tablet_recorder[21], 1);
}

} // namespace doris