// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/grpc_impl/GrpcRequestHandler.h"

#include <fiu/fiu-local.h>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "query/QueryUtil.h"
#include "server/ValidationUtil.h"
#include "server/context/ConnectionContext.h"
#include "tracing/TextMapCarrier.h"
#include "tracing/TracerUtil.h"
#include "utils/CommonUtil.h"
#include "utils/Log.h"
#include "value/config/ServerConfig.h"

namespace milvus {
namespace server {
namespace grpc {

const char* EXTRA_PARAM_KEY = "params";
const size_t MAXIMUM_FIELD_NUM = 64;

::milvus::grpc::ErrorCode
ErrorMap(ErrorCode code) {
    static const std::map<ErrorCode, ::milvus::grpc::ErrorCode> code_map = {
        {SERVER_UNEXPECTED_ERROR, ::milvus::grpc::ErrorCode::UNEXPECTED_ERROR},
        {SERVER_UNSUPPORTED_ERROR, ::milvus::grpc::ErrorCode::UNEXPECTED_ERROR},
        {SERVER_NULL_POINTER, ::milvus::grpc::ErrorCode::UNEXPECTED_ERROR},
        {SERVER_INVALID_ARGUMENT, ::milvus::grpc::ErrorCode::ILLEGAL_ARGUMENT},
        {SERVER_FILE_NOT_FOUND, ::milvus::grpc::ErrorCode::FILE_NOT_FOUND},
        {SERVER_NOT_IMPLEMENT, ::milvus::grpc::ErrorCode::UNEXPECTED_ERROR},
        {SERVER_CANNOT_CREATE_FOLDER, ::milvus::grpc::ErrorCode::CANNOT_CREATE_FOLDER},
        {SERVER_CANNOT_CREATE_FILE, ::milvus::grpc::ErrorCode::CANNOT_CREATE_FILE},
        {SERVER_CANNOT_DELETE_FOLDER, ::milvus::grpc::ErrorCode::CANNOT_DELETE_FOLDER},
        {SERVER_CANNOT_DELETE_FILE, ::milvus::grpc::ErrorCode::CANNOT_DELETE_FILE},
        {SERVER_COLLECTION_NOT_EXIST, ::milvus::grpc::ErrorCode::COLLECTION_NOT_EXISTS},
        {SERVER_INVALID_COLLECTION_NAME, ::milvus::grpc::ErrorCode::ILLEGAL_COLLECTION_NAME},
        {SERVER_INVALID_COLLECTION_DIMENSION, ::milvus::grpc::ErrorCode::ILLEGAL_DIMENSION},
        {SERVER_INVALID_VECTOR_DIMENSION, ::milvus::grpc::ErrorCode::ILLEGAL_DIMENSION},
        {SERVER_INVALID_FIELD_NAME, ::milvus::grpc::ErrorCode::ILLEGAL_ARGUMENT},
        {SERVER_INVALID_FIELD_NUM, ::milvus::grpc::ErrorCode::ILLEGAL_ARGUMENT},

        {SERVER_INVALID_INDEX_TYPE, ::milvus::grpc::ErrorCode::ILLEGAL_INDEX_TYPE},
        {SERVER_INVALID_ROWRECORD, ::milvus::grpc::ErrorCode::ILLEGAL_ROWRECORD},
        {SERVER_INVALID_ROWRECORD_ARRAY, ::milvus::grpc::ErrorCode::ILLEGAL_ROWRECORD},
        {SERVER_INVALID_TOPK, ::milvus::grpc::ErrorCode::ILLEGAL_TOPK},
        {SERVER_INVALID_NPROBE, ::milvus::grpc::ErrorCode::ILLEGAL_ARGUMENT},
        {SERVER_INVALID_INDEX_NLIST, ::milvus::grpc::ErrorCode::ILLEGAL_NLIST},
        {SERVER_INVALID_INDEX_METRIC_TYPE, ::milvus::grpc::ErrorCode::ILLEGAL_METRIC_TYPE},
        {SERVER_INVALID_SEGMENT_ROW_COUNT, ::milvus::grpc::ErrorCode::ILLEGAL_ARGUMENT},
        {SERVER_ILLEGAL_VECTOR_ID, ::milvus::grpc::ErrorCode::ILLEGAL_VECTOR_ID},
        {SERVER_ILLEGAL_SEARCH_RESULT, ::milvus::grpc::ErrorCode::ILLEGAL_SEARCH_RESULT},
        {SERVER_CACHE_FULL, ::milvus::grpc::ErrorCode::CACHE_FAILED},
        {DB_META_TRANSACTION_FAILED, ::milvus::grpc::ErrorCode::META_FAILED},
        {SERVER_BUILD_INDEX_ERROR, ::milvus::grpc::ErrorCode::BUILD_INDEX_ERROR},
        {SERVER_OUT_OF_MEMORY, ::milvus::grpc::ErrorCode::OUT_OF_MEMORY},
    };

    if (code_map.find(code) != code_map.end()) {
        return code_map.at(code);
    } else {
        return ::milvus::grpc::ErrorCode::UNEXPECTED_ERROR;
    }
}

std::string
RequestMap(ReqType req_type) {
    static const std::unordered_map<ReqType, std::string> req_map = {
        {ReqType::kInsert, "Insert"}, {ReqType::kCreateIndex, "CreateIndex"},     {ReqType::kSearch, "Search"},
        {ReqType::kFlush, "Flush"},   {ReqType::kGetEntityByID, "GetEntityByID"}, {ReqType::kCompact, "Compact"},
    };

    if (req_map.find(req_type) != req_map.end()) {
        return req_map.at(req_type);
    } else {
        return "OtherReq";
    }
}

namespace {
template <typename T>
void
RecordDataAddr(const std::string& field_name, int32_t num, const T* data, InsertParam& insert_param) {
    int64_t bytes = num * sizeof(T);
    const char* data_addr = reinterpret_cast<const char*>(data);
    auto data_segment = std::make_pair(data_addr, bytes);
    insert_param.fields_data_[field_name].emplace_back(data_segment);
}

void
RecordVectorDataAddr(const std::string& field_name,
                     const google::protobuf::RepeatedPtrField<::milvus::grpc::VectorRowRecord>& grpc_records,
                     InsertParam& insert_param) {
    // calculate data size
    int64_t float_data_size = 0, binary_data_size = 0;
    for (auto& record : grpc_records) {
        float_data_size += record.float_data_size();
        binary_data_size += record.binary_data().size();
    }

    if (float_data_size > 0) {
        for (auto& record : grpc_records) {
            RecordDataAddr<float>(field_name, record.float_data_size(), record.float_data().data(), insert_param);
        }
    } else if (binary_data_size > 0) {
        for (auto& record : grpc_records) {
            RecordDataAddr<char>(field_name, record.binary_data().size(), record.binary_data().data(), insert_param);
        }
    }
}

void
CopyRowRecords(const google::protobuf::RepeatedPtrField<::milvus::grpc::VectorRowRecord>& grpc_records,
               const google::protobuf::RepeatedField<google::protobuf::int64>& grpc_id_array,
               engine::VectorsData& vectors) {
    // step 1: copy vector data
    int64_t float_data_size = 0, binary_data_size = 0;
    for (auto& record : grpc_records) {
        float_data_size += record.float_data_size();
        binary_data_size += record.binary_data().size();
    }

    std::vector<float> float_array(float_data_size, 0.0f);
    std::vector<uint8_t> binary_array(binary_data_size, 0);
    int64_t offset = 0;
    if (float_data_size > 0) {
        for (auto& record : grpc_records) {
            memcpy(&float_array[offset], record.float_data().data(), record.float_data_size() * sizeof(float));
            offset += record.float_data_size();
        }
    } else if (binary_data_size > 0) {
        for (auto& record : grpc_records) {
            memcpy(&binary_array[offset], record.binary_data().data(), record.binary_data().size());
            offset += record.binary_data().size();
        }
    }

    // step 2: copy id array
    std::vector<int64_t> id_array;
    if (grpc_id_array.size() > 0) {
        id_array.resize(grpc_id_array.size());
        memcpy(id_array.data(), grpc_id_array.data(), grpc_id_array.size() * sizeof(int64_t));
    }

    // step 3: contruct vectors
    vectors.vector_count_ = grpc_records.size();
    vectors.float_data_.swap(float_array);
    vectors.binary_data_.swap(binary_array);
    vectors.id_array_.swap(id_array);
}

void
DeSerialization(const ::milvus::grpc::GeneralQuery& general_query, query::BooleanQueryPtr& boolean_clause,
                query::QueryPtr& query_ptr) {
    if (general_query.has_boolean_query()) {
        boolean_clause->SetOccur(static_cast<query::Occur>(general_query.boolean_query().occur()));
        for (uint64_t i = 0; i < general_query.boolean_query().general_query_size(); ++i) {
            if (general_query.boolean_query().general_query(i).has_boolean_query()) {
                query::BooleanQueryPtr query = std::make_shared<query::BooleanQuery>();
                DeSerialization(general_query.boolean_query().general_query(i), query, query_ptr);
                boolean_clause->AddBooleanQuery(query);
            } else {
                auto leaf_query = std::make_shared<query::LeafQuery>();
                auto query = general_query.boolean_query().general_query(i);
                //                if (query.has_term_query()) {
                //                    query::TermQueryPtr term_query = std::make_shared<query::TermQuery>();
                //                    term_query->field_name = query.term_query().field_name();
                //                    term_query->boost = query.term_query().boost();
                //                    size_t int_size = query.term_query().int_value_size();
                //                    size_t double_size = query.term_query().double_value_size();
                //                    if (int_size > 0) {
                //                        term_query->field_value.resize(int_size * sizeof(int64_t));
                //                        memcpy(term_query->field_value.data(), query.term_query().int_value().data(),
                //                               int_size * sizeof(int64_t));
                //                    } else if (double_size > 0) {
                //                        term_query->field_value.resize(double_size * sizeof(double));
                //                        memcpy(term_query->field_value.data(),
                //                        query.term_query().double_value().data(),
                //                               double_size * sizeof(double));
                //                    }
                //                    leaf_query->term_query = term_query;
                //                    boolean_clause->AddLeafQuery(leaf_query);
                //                }
                //                if (query.has_range_query()) {
                //                    query::RangeQueryPtr range_query = std::make_shared<query::RangeQuery>();
                //                    range_query->field_name = query.range_query().field_name();
                //                    range_query->boost = query.range_query().boost();
                //                    range_query->compare_expr.resize(query.range_query().operand_size());
                //                    for (uint64_t j = 0; j < query.range_query().operand_size(); ++j) {
                //                        range_query->compare_expr[j].compare_operator =
                //                            query::CompareOperator(query.range_query().operand(j).operator_());
                //                        range_query->compare_expr[j].operand =
                //                        query.range_query().operand(j).operand();
                //                    }
                //                    leaf_query->range_query = range_query;
                //                    boolean_clause->AddLeafQuery(leaf_query);
                //                }
                if (query.has_vector_query()) {
                    query::VectorQueryPtr vector_query = std::make_shared<query::VectorQuery>();

                    engine::VectorsData vectors;
                    CopyRowRecords(query.vector_query().records(),
                                   google::protobuf::RepeatedField<google::protobuf::int64>(), vectors);

                    vector_query->query_vector.vector_count = vectors.vector_count_;
                    vector_query->query_vector.float_data.swap(vectors.float_data_);
                    vector_query->query_vector.binary_data.swap(vectors.binary_data_);

                    vector_query->boost = query.vector_query().query_boost();
                    vector_query->field_name = query.vector_query().field_name();
                    vector_query->topk = query.vector_query().topk();

                    milvus::json json_params;
                    for (int j = 0; j < query.vector_query().extra_params_size(); j++) {
                        const ::milvus::grpc::KeyValuePair& extra = query.vector_query().extra_params(j);
                        if (extra.key() == EXTRA_PARAM_KEY) {
                            json_params = json::parse(extra.value());
                        }
                    }
                    vector_query->extra_params = json_params;

                    // TODO(yukun): remove hardcode here
                    std::string vector_placeholder = "placeholder_1";
                    query_ptr->vectors.insert(std::make_pair(vector_placeholder, vector_query));

                    leaf_query->vector_placeholder = vector_placeholder;
                    boolean_clause->AddLeafQuery(leaf_query);
                }
            }
        }
    }
}

void
ConstructResults(const TopKQueryResult& result, ::milvus::grpc::QueryResult* response) {
    if (!response) {
        return;
    }

    response->set_row_num(result.row_num_);

    response->mutable_entities()->mutable_ids()->Resize(static_cast<int>(result.id_list_.size()), 0);
    memcpy(response->mutable_entities()->mutable_ids()->mutable_data(), result.id_list_.data(),
           result.id_list_.size() * sizeof(int64_t));

    response->mutable_distances()->Resize(static_cast<int>(result.distance_list_.size()), 0.0);
    memcpy(response->mutable_distances()->mutable_data(), result.distance_list_.data(),
           result.distance_list_.size() * sizeof(float));
}

void
CopyDataChunkToEntity(const engine::DataChunkPtr& data_chunk,
                      const engine::snapshot::FieldElementMappings& field_mappings, int64_t id_size,
                      ::milvus::grpc::Entities* response) {
    if (data_chunk == nullptr) {
        return;
    }

    for (const auto& it : field_mappings) {
        auto type = it.first->GetFtype();
        std::string name = it.first->GetName();

        // judge whether data exists
        engine::BinaryDataPtr data = data_chunk->fixed_fields_[name];
        if (data == nullptr || data->data_.empty()) {
            continue;
        }

        auto single_size = (id_size != 0) ? (data->data_.size() / id_size) : 0;

        auto field_value = response->add_fields();
        auto vector_record = field_value->mutable_vector_record();

        field_value->set_field_name(name);
        field_value->set_type(static_cast<milvus::grpc::DataType>(type));
        // general data
        if (type == engine::DataType::VECTOR_BINARY) {
            // add binary vector data
            std::vector<int8_t> binary_vector;
            auto vector_size = single_size * sizeof(int8_t) / sizeof(int8_t);
            binary_vector.resize(vector_size);
            for (int i = 0; i < id_size; i++) {
                auto vector_row_record = vector_record->add_records();
                auto offset = i * single_size;
                memcpy(binary_vector.data(), data->data_.data() + offset, single_size);
                vector_row_record->mutable_binary_data()->resize(binary_vector.size());
                memcpy(vector_row_record->mutable_binary_data()->data(), binary_vector.data(), binary_vector.size());
            }

        } else if (type == engine::DataType::VECTOR_FLOAT) {
            // add float vector data
            std::vector<float> float_vector;
            auto vector_size = single_size * sizeof(int8_t) / sizeof(float);
            float_vector.resize(vector_size);
            for (int i = 0; i < id_size; i++) {
                auto vector_row_record = vector_record->add_records();
                auto offset = i * single_size;
                memcpy(float_vector.data(), data->data_.data() + offset, single_size);
                vector_row_record->mutable_float_data()->Resize(vector_size, 0.0);
                memcpy(vector_row_record->mutable_float_data()->mutable_data(), float_vector.data(),
                       float_vector.size() * sizeof(float));
            }
        } else {
            // add attribute data
            auto attr_record = field_value->mutable_attr_record();
            if (type == engine::DataType::INT32) {
                // add int32 data
                int32_t int32_value;
                for (int i = 0; i < id_size; i++) {
                    auto offset = i * single_size;
                    memcpy(&int32_value, data->data_.data() + offset, single_size);
                    attr_record->add_int32_value(int32_value);
                }
            } else if (type == engine::DataType::INT64) {
                // add int64 data
                int64_t int64_value;
                for (int i = 0; i < id_size; i++) {
                    auto offset = i * single_size;
                    memcpy(&int64_value, data->data_.data() + offset, single_size);
                    attr_record->add_int64_value(int64_value);
                }
            } else if (type == engine::DataType::DOUBLE) {
                // add double data
                double double_value;
                for (int i = 0; i < id_size; i++) {
                    auto offset = i * single_size;
                    memcpy(&double_value, data->data_.data() + offset, single_size);
                    attr_record->add_double_value(double_value);
                }
            } else if (type == engine::DataType::FLOAT) {
                // add float data
                float float_value;
                for (int i = 0; i < id_size; i++) {
                    auto offset = i * single_size;
                    memcpy(&float_value, data->data_.data() + offset, single_size);
                    attr_record->add_float_value(float_value);
                }
            }
        }
    }
}

void
ConstructEntityResults(const std::vector<engine::AttrsData>& attrs, const std::vector<engine::VectorsData>& vectors,
                       std::vector<std::string>& field_names, ::milvus::grpc::Entities* response) {
    if (!response) {
        return;
    }

    auto id_size = vectors.size();
    std::vector<int64_t> id_array(id_size);
    for (int64_t i = 0; i < id_size; i++) {
        id_array[i] = vectors[i].id_array_[0];
    }
    response->mutable_ids()->Resize(static_cast<int>(id_size), 0);
    memcpy(response->mutable_ids()->mutable_data(), id_array.data(), id_size * sizeof(int64_t));

    std::string vector_field_name;
    bool set_valid_row = false;
    for (const auto& field_name : field_names) {
        if (!attrs.empty()) {
            if (attrs[0].attr_type_.find(field_name) != attrs[0].attr_type_.end()) {
                auto grpc_field = response->add_fields();
                grpc_field->set_field_name(field_name);
                grpc_field->set_type(static_cast<::milvus::grpc::DataType>(attrs[0].attr_type_.at(field_name)));
                auto grpc_attr_data = grpc_field->mutable_attr_record();

                std::vector<int32_t> int32_data;
                std::vector<int64_t> int64_data;
                std::vector<float> float_data;
                std::vector<double> double_data;
                for (auto& attr : attrs) {
                    if (not set_valid_row) {
                        if (!attr.id_array_.empty()) {
                            response->add_valid_row(true);
                        } else {
                            response->add_valid_row(false);
                            continue;
                        }
                    }

                    if (attr.attr_data_.find(field_name) == attr.attr_data_.end()) {
                        continue;
                    }
                    auto attr_data = attr.attr_data_.at(field_name);
                    int32_t grpc_int32_data;
                    int64_t grpc_int64_data;
                    float grpc_float_data;
                    double grpc_double_data;
                    switch (attr.attr_type_.at(field_name)) {
                        case engine::DataType::INT8: {
                            if (attr_data.size() == sizeof(int8_t)) {
                                grpc_int32_data = attr_data[0];
                                int32_data.emplace_back(grpc_int32_data);
                            } else {
                                response->mutable_status()->set_error_code(::milvus::grpc::ErrorCode::UNEXPECTED_ERROR);
                                return;
                            }
                            break;
                        }
                        case engine::DataType::INT16: {
                            if (attr_data.size() == sizeof(int16_t)) {
                                int16_t value;
                                memcpy(&value, attr_data.data(), sizeof(int16_t));
                                grpc_int32_data = value;
                                int32_data.emplace_back(grpc_int32_data);
                            } else {
                                response->mutable_status()->set_error_code(::milvus::grpc::ErrorCode::UNEXPECTED_ERROR);
                                return;
                            }
                            break;
                        }
                        case engine::DataType::INT32: {
                            if (attr_data.size() == sizeof(int32_t)) {
                                memcpy(&grpc_int32_data, attr_data.data(), sizeof(int32_t));
                                int32_data.emplace_back(grpc_int32_data);
                            } else {
                                response->mutable_status()->set_error_code(::milvus::grpc::ErrorCode::UNEXPECTED_ERROR);
                                return;
                            }
                            break;
                        }
                        case engine::DataType::INT64: {
                            if (attr_data.size() == sizeof(int64_t)) {
                                memcpy(&grpc_int64_data, attr_data.data(), sizeof(int64_t));
                                int64_data.emplace_back(grpc_int64_data);
                            } else {
                                response->mutable_status()->set_error_code(::milvus::grpc::ErrorCode::UNEXPECTED_ERROR);
                                return;
                            }
                            break;
                        }
                        case engine::DataType::FLOAT: {
                            if (attr_data.size() == sizeof(float)) {
                                float value;
                                memcpy(&value, attr_data.data(), sizeof(float));
                                grpc_float_data = value;
                                float_data.emplace_back(grpc_float_data);
                            } else {
                                response->mutable_status()->set_error_code(::milvus::grpc::ErrorCode::UNEXPECTED_ERROR);
                                return;
                            }
                            break;
                        }
                        case engine::DataType::DOUBLE: {
                            if (attr_data.size() == sizeof(double)) {
                                memcpy(&grpc_double_data, attr_data.data(), sizeof(double));
                                double_data.emplace_back(grpc_double_data);
                            } else {
                                response->mutable_status()->set_error_code(::milvus::grpc::ErrorCode::UNEXPECTED_ERROR);
                                return;
                            }
                            break;
                        }
                        default: { break; }
                    }
                }
                if (!int32_data.empty()) {
                    grpc_attr_data->mutable_int32_value()->Resize(static_cast<int>(int32_data.size()), 0);
                    memcpy(grpc_attr_data->mutable_int32_value()->mutable_data(), int32_data.data(),
                           int32_data.size() * sizeof(int32_t));
                } else if (!int64_data.empty()) {
                    grpc_attr_data->mutable_int64_value()->Resize(static_cast<int>(int64_data.size()), 0);
                    memcpy(grpc_attr_data->mutable_int64_value()->mutable_data(), int64_data.data(),
                           int64_data.size() * sizeof(int64_t));
                } else if (!float_data.empty()) {
                    grpc_attr_data->mutable_float_value()->Resize(static_cast<int>(float_data.size()), 0.0);
                    memcpy(grpc_attr_data->mutable_float_value()->mutable_data(), float_data.data(),
                           float_data.size() * sizeof(float));
                } else if (!double_data.empty()) {
                    grpc_attr_data->mutable_double_value()->Resize(static_cast<int>(double_data.size()), 0.0);
                    memcpy(grpc_attr_data->mutable_double_value()->mutable_data(), double_data.data(),
                           double_data.size() * sizeof(double));
                }
                set_valid_row = true;
            } else {
                vector_field_name = field_name;
            }
        }
    }

    // TODO(yukun): valid_row not used in vector records serialize
    if (!vector_field_name.empty()) {
        auto grpc_field = response->add_fields();
        grpc_field->set_field_name(vector_field_name);
        ::milvus::grpc::VectorRecord* grpc_vector_data = grpc_field->mutable_vector_record();
        for (auto& vector : vectors) {
            auto grpc_data = grpc_vector_data->add_records();
            if (!vector.float_data_.empty()) {
                if (not set_valid_row) {
                    response->add_valid_row(true);
                }
                grpc_field->set_type(::milvus::grpc::DataType::VECTOR_FLOAT);
                grpc_data->mutable_float_data()->Resize(vector.float_data_.size(), 0);
                memcpy(grpc_data->mutable_float_data()->mutable_data(), vector.float_data_.data(),
                       vector.float_data_.size() * sizeof(float));
            } else if (!vector.binary_data_.empty()) {
                if (not set_valid_row) {
                    response->add_valid_row(true);
                }
                grpc_field->set_type(::milvus::grpc::DataType::VECTOR_BINARY);
                grpc_data->mutable_binary_data()->resize(vector.binary_data_.size());
                memcpy(grpc_data->mutable_binary_data()->data(), vector.binary_data_.data(),
                       vector.binary_data_.size() * sizeof(uint8_t));
            } else {
                if (not set_valid_row) {
                    response->add_valid_row(false);
                }
            }
        }
    }
}

class GrpcConnectionContext : public milvus::server::ConnectionContext {
 public:
    explicit GrpcConnectionContext(::grpc::ServerContext* context) : context_(context) {
    }

    bool
    IsConnectionBroken() const override {
        if (context_ == nullptr) {
            return true;
        }

        return context_->IsCancelled();
    }

 private:
    ::grpc::ServerContext* context_ = nullptr;
};

}  // namespace

namespace {

#define REQ_ID ("request_id")

std::atomic<int64_t> _sequential_id;

int64_t
get_sequential_id() {
    return _sequential_id++;
}

void
set_request_id(::grpc::ServerContext* context, const std::string& request_id) {
    if (not context) {
        // error
        LOG_SERVER_ERROR_ << "set_request_id: grpc::ServerContext is nullptr" << std::endl;
        return;
    }

    context->AddInitialMetadata(REQ_ID, request_id);
}

std::string
get_request_id(::grpc::ServerContext* context) {
    if (not context) {
        // error
        LOG_SERVER_ERROR_ << "get_request_id: grpc::ServerContext is nullptr" << std::endl;
        return "INVALID_ID";
    }

    auto server_metadata = context->server_metadata();

    auto request_id_kv = server_metadata.find(REQ_ID);
    if (request_id_kv == server_metadata.end()) {
        // error
        LOG_SERVER_ERROR_ << std::string(REQ_ID) << " not found in grpc.server_metadata" << std::endl;
        return "INVALID_ID";
    }

    return request_id_kv->second.data();
}

}  // namespace

GrpcRequestHandler::GrpcRequestHandler(const std::shared_ptr<opentracing::Tracer>& tracer)
    : tracer_(tracer),
      random_num_generator_(),
      max_concurrent_insert_request_size_(config.cache.max_concurrent_insert_request_size()),
      max_concurrent_insert_request_size(max_concurrent_insert_request_size_) {
    std::random_device random_device;
    random_num_generator_.seed(random_device());
}

void
GrpcRequestHandler::OnPostRecvInitialMetaData(
    ::grpc::experimental::ServerRpcInfo* server_rpc_info,
    ::grpc::experimental::InterceptorBatchMethods* interceptor_batch_methods) {
    std::unordered_map<std::string, std::string> text_map;
    auto* metadata_map = interceptor_batch_methods->GetRecvInitialMetadata();
    auto context_kv = metadata_map->find(tracing::TracerUtil::GetTraceContextHeaderName());
    if (context_kv != metadata_map->end()) {
        text_map[std::string(context_kv->first.data(), context_kv->first.length())] =
            std::string(context_kv->second.data(), context_kv->second.length());
    }
    // test debug mode
    //    if (std::string(server_rpc_info->method()).find("Search") != std::string::npos) {
    //        text_map["demo-debug-id"] = "debug-id";
    //    }

    tracing::TextMapCarrier carrier{text_map};
    auto span_context_maybe = tracer_->Extract(carrier);
    if (!span_context_maybe) {
        std::cerr << span_context_maybe.error().message() << std::endl;
        return;
    }
    auto span = tracer_->StartSpan(server_rpc_info->method(), {opentracing::ChildOf(span_context_maybe->get())});

    auto server_context = server_rpc_info->server_context();
    auto client_metadata = server_context->client_metadata();

    // if client provide request_id in metadata, milvus just use it,
    // else milvus generate a sequential id.
    std::string request_id;
    auto request_id_kv = client_metadata.find("request_id");
    if (request_id_kv != client_metadata.end()) {
        request_id = request_id_kv->second.data();
        LOG_SERVER_DEBUG_ << "client provide request_id: " << request_id;

        // if request_id is being used by another request,
        // convert it to request_id_n.
        std::lock_guard<std::mutex> lock(context_map_mutex_);
        if (context_map_.find(request_id) == context_map_.end()) {
            // if not found exist, mark
            context_map_[request_id] = nullptr;
        } else {
            // Finding a unused suffix
            int64_t suffix = 1;
            std::string try_request_id;
            bool exist = true;
            do {
                try_request_id = request_id + "_" + std::to_string(suffix);
                exist = context_map_.find(try_request_id) != context_map_.end();
                suffix++;
            } while (exist);
            context_map_[try_request_id] = nullptr;
        }
    } else {
        request_id = std::to_string(get_sequential_id());
        set_request_id(server_context, request_id);
        LOG_SERVER_DEBUG_ << "milvus generate request_id: " << request_id;
    }

    auto trace_context = std::make_shared<tracing::TraceContext>(span);
    auto context = std::make_shared<Context>(request_id);
    context->SetTraceContext(trace_context);
    SetContext(server_rpc_info->server_context(), context);
}

void
GrpcRequestHandler::OnPreSendMessage(::grpc::experimental::ServerRpcInfo* server_rpc_info,
                                     ::grpc::experimental::InterceptorBatchMethods* interceptor_batch_methods) {
    rpc_requests_total_counter_.Increment();
    std::lock_guard<std::mutex> lock(context_map_mutex_);
    auto request_id = get_request_id(server_rpc_info->server_context());

    if (context_map_.find(request_id) == context_map_.end()) {
        // error
        LOG_SERVER_ERROR_ << "request_id " << request_id << " not found in context_map_";
        return;
    }
    context_map_[request_id]->GetTraceContext()->GetSpan()->Finish();
    context_map_.erase(request_id);
}

std::shared_ptr<Context>
GrpcRequestHandler::GetContext(::grpc::ServerContext* server_context) {
    std::lock_guard<std::mutex> lock(context_map_mutex_);
    auto request_id = get_request_id(server_context);

    auto iter = context_map_.find(request_id);
    if (iter == context_map_.end()) {
        LOG_SERVER_ERROR_ << "GetContext: request_id " << request_id << " not found in context_map_";
        return nullptr;
    }

    if (iter->second != nullptr) {
        ConnectionContextPtr connection_context = std::make_shared<GrpcConnectionContext>(server_context);
        iter->second->SetConnectionContext(connection_context);
    }
    return iter->second;
}

void
GrpcRequestHandler::SetContext(::grpc::ServerContext* server_context, const std::shared_ptr<Context>& context) {
    std::lock_guard<std::mutex> lock(context_map_mutex_);
    auto request_id = get_request_id(server_context);
    context_map_[request_id] = context;
}

uint64_t
GrpcRequestHandler::random_id() const {
    std::lock_guard<std::mutex> lock(random_mutex_);
    auto value = random_num_generator_();
    while (value == 0) {
        value = random_num_generator_();
    }
    return value;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

::grpc::Status
GrpcRequestHandler::CreateCollection(::grpc::ServerContext* context, const ::milvus::grpc::Mapping* request,
                                     ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::unordered_map<std::string, FieldSchema> fields;

    if (request->fields_size() > MAXIMUM_FIELD_NUM) {
        Status status = Status{SERVER_INVALID_FIELD_NUM, "Maximum field's number should be limited to 64"};
        LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
        SET_RESPONSE(response, status, context);
        return ::grpc::Status::OK;
    }

    for (int i = 0; i < request->fields_size(); ++i) {
        const auto& field = request->fields(i);

        if (fields.find(field.name()) != fields.end()) {
            auto status = Status(SERVER_INVALID_FIELD_NAME, "Collection mapping has duplicate field name");
            SET_RESPONSE(response, status, context)
            return ::grpc::Status::OK;
        }

        FieldSchema field_schema;
        field_schema.field_type_ = static_cast<engine::DataType>(field.type());

        // Currently only one extra_param
        if (field.extra_params_size() != 0) {
            if (!field.extra_params(0).value().empty()) {
                field_schema.field_params_ = json::parse(field.extra_params(0).value());
            }
        }

        for (int j = 0; j < field.index_params_size(); j++) {
            field_schema.index_params_[field.index_params(j).key()] = field.index_params(j).value();
        }

        fields[field.name()] = field_schema;
    }

    milvus::json json_params;
    for (int i = 0; i < request->extra_params_size(); i++) {
        const ::milvus::grpc::KeyValuePair& extra = request->extra_params(i);
        if (extra.key() == EXTRA_PARAM_KEY) {
            json_params = json::parse(extra.value());
        }
    }

    Status status = req_handler_.CreateCollection(GetContext(context), request->collection_name(), fields, json_params);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context)

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::HasCollection(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                  ::milvus::grpc::BoolReply* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    bool has_collection = false;

    Status status = req_handler_.HasCollection(GetContext(context), request->collection_name(), has_collection);
    response->set_bool_reply(has_collection);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::DropCollection(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                   ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status = req_handler_.DropCollection(GetContext(context), request->collection_name());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);
    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::CreateIndex(::grpc::ServerContext* context, const ::milvus::grpc::IndexParam* request,
                                ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request)
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    milvus::json json_params;
    for (int i = 0; i < request->extra_params_size(); i++) {
        const ::milvus::grpc::KeyValuePair& extra = request->extra_params(i);
        if (extra.key() == EXTRA_PARAM_KEY) {
            json_params[EXTRA_PARAM_KEY] = json::parse(extra.value());
        } else {
            json_params[extra.key()] = extra.value();
        }
    }

    Status status = req_handler_.CreateIndex(GetContext(context), request->collection_name(), request->field_name(),
                                             request->index_name(), json_params);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);
    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::DescribeIndex(::grpc::ServerContext* context, const ::milvus::grpc::IndexParam* request,
                                  ::milvus::grpc::IndexParam* response) {
    CHECK_NULLPTR_RETURN(request)
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::string index_name;
    milvus::json index_params;
    Status status = req_handler_.DescribeIndex(GetContext(context), request->collection_name(), request->field_name(),
                                               index_name, index_params);

    response->set_collection_name(request->collection_name());
    response->set_field_name(request->field_name());
    ::milvus::grpc::KeyValuePair* kv = response->add_extra_params();
    kv->set_key(EXTRA_PARAM_KEY);
    kv->set_value(index_params.dump());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);
    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::DropIndex(::grpc::ServerContext* context, const ::milvus::grpc::IndexParam* request,
                              ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status = req_handler_.DropIndex(GetContext(context), request->collection_name(), request->field_name(),
                                           request->index_name());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::GetEntityByID(::grpc::ServerContext* context, const ::milvus::grpc::EntityIdentity* request,
                                  ::milvus::grpc::Entities* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    engine::IDNumbers vector_ids;
    vector_ids.reserve(request->id_array_size());
    for (int64_t i = 0; i < request->id_array_size(); i++) {
        vector_ids.push_back(request->id_array(i));
    }

    std::vector<std::string> field_names(request->field_names_size());
    for (int64_t i = 0; i < request->field_names_size(); i++) {
        field_names[i] = request->field_names(i);
    }

    engine::DataChunkPtr data_chunk;
    engine::snapshot::FieldElementMappings field_mappings;

    std::vector<bool> valid_row;

    Status status = req_handler_.GetEntityByID(GetContext(context), request->collection_name(), vector_ids, field_names,
                                               valid_row, field_mappings, data_chunk);
    for (auto it : vector_ids) {
        response->add_ids(it);
    }

    int64_t valid_size = 0;
    for (auto it : valid_row) {
        response->add_valid_row(it);
        if (it) {
            valid_size++;
        }
    }

    CopyDataChunkToEntity(data_chunk, field_mappings, valid_size, response);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::GetEntityIDs(::grpc::ServerContext* context, const ::milvus::grpc::GetEntityIDsParam* request,
                                 ::milvus::grpc::EntityIds* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::vector<int64_t> vector_ids;
    Status status = req_handler_.ListIDInSegment(GetContext(context), request->collection_name(), request->segment_id(),
                                                 vector_ids);

    if (!vector_ids.empty()) {
        response->mutable_entity_id_array()->Resize(vector_ids.size(), -1);
        memcpy(response->mutable_entity_id_array()->mutable_data(), vector_ids.data(),
               vector_ids.size() * sizeof(int64_t));
    }

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

//::grpc::Status
// GrpcRequestHandler::Search(::grpc::ServerContext* context, const ::milvus::grpc::SearchParam* request,
//                           ::milvus::grpc::QueryResult* response) {
//    CHECK_NULLPTR_RETURN(request);
//    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);
//
//    // step 1: copy vector data
//    engine::VectorsData vectors;
//    CopyRowRecords(request->query_record_array(), google::protobuf::RepeatedField<google::protobuf::int64>(),
//    vectors);
//
//    // step 2: partition tags
//    std::vector<std::string> partitions;
//    std::copy(request->partition_tag_array().begin(), request->partition_tag_array().end(),
//              std::back_inserter(partitions));
//
//    // step 3: parse extra parameters
//    milvus::json json_params;
//    for (int i = 0; i < request->extra_params_size(); i++) {
//        const ::milvus::grpc::KeyValuePair& extra = request->extra_params(i);
//        if (extra.key() == EXTRA_PARAM_KEY) {
//            json_params = json::parse(extra.value());
//        }
//    }
//
//    // step 4: search vectors
//    std::vector<std::string> file_ids;
//    TopKQueryResult result;
//    fiu_do_on("GrpcRequestHandler.Search.not_empty_file_ids", file_ids.emplace_back("test_file_id"));
//
//    Status status = req_handler_.Search(GetContext(context), request->collection_name(), vectors, request->topk(),
//                                            json_params, partitions, file_ids, result);
//
//    // step 5: construct and return result
//    ConstructResults(result, response);
//
//    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
//    SET_RESPONSE(response->mutable_status(), status, context);
//
//    return ::grpc::Status::OK;
//}

::grpc::Status
GrpcRequestHandler::SearchInSegment(::grpc::ServerContext* context, const ::milvus::grpc::SearchInSegmentParam* request,
                                    ::milvus::grpc::QueryResult* response) {
    //    CHECK_NULLPTR_RETURN(request);
    //    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);
    //
    //    auto* search_request = &request->search_param();
    //
    //    // step 1: copy vector data
    //    engine::VectorsData vectors;
    //    CopyRowRecords(search_request->query_record_array(),
    //    google::protobuf::RepeatedField<google::protobuf::int64>(),
    //                   vectors);
    //
    //    // step 2: copy file id array
    //    std::vector<std::string> file_ids;
    //    std::copy(request->file_id_array().begin(), request->file_id_array().end(), std::back_inserter(file_ids));
    //
    //    // step 3: partition tags
    //    std::vector<std::string> partitions;
    //    std::copy(search_request->partition_tag_array().begin(), search_request->partition_tag_array().end(),
    //              std::back_inserter(partitions));
    //
    //    // step 4: parse extra parameters
    //    milvus::json json_params;
    //    for (int i = 0; i < search_request->extra_params_size(); i++) {
    //        const ::milvus::grpc::KeyValuePair& extra = search_request->extra_params(i);
    //        if (extra.key() == EXTRA_PARAM_KEY) {
    //            json_params = json::parse(extra.value());
    //        }
    //    }
    //
    //    // step 5: search vectors
    //    TopKQueryResult result;
    //    Status status = req_handler_.Search(GetContext(context), search_request->collection_name(), vectors,
    //                                            search_request->topk(), json_params, partitions, file_ids, result);
    //
    //    // step 6: construct and return result
    //    ConstructResults(result, response);
    //
    //    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    //    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::DescribeCollection(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                       ::milvus::grpc::Mapping* response) {
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);
    CHECK_NULLPTR_RETURN(request);
    try {
        milvus::server::CollectionSchema collection_schema;
        Status status =
            req_handler_.GetCollectionInfo(GetContext(context), request->collection_name(), collection_schema);
        if (!status.ok()) {
            SET_RESPONSE(response->mutable_status(), status, context);
            return ::grpc::Status::OK;
        }

        response->set_collection_name(request->collection_name());
        for (auto& field_kv : collection_schema.fields_) {
            if (field_kv.first == engine::FIELD_UID) {
                continue;
            }
            auto field = response->add_fields();
            auto& field_name = field_kv.first;
            auto& field_schema = field_kv.second;

            field->set_name(field_name);
            field->set_type(static_cast<milvus::grpc::DataType>(field_schema.field_type_));

            auto grpc_field_param = field->add_extra_params();
            grpc_field_param->set_key(EXTRA_PARAM_KEY);
            grpc_field_param->set_value(field_schema.field_params_.dump());

            for (auto& item : field_schema.index_params_.items()) {
                auto grpc_index_param = field->add_index_params();
                grpc_index_param->set_key(item.key());
                if (item.value().is_object()) {
                    grpc_index_param->set_value(item.value().dump());
                } else {
                    grpc_index_param->set_value(item.value());
                }
            }
        }

        auto grpc_extra_param = response->add_extra_params();
        grpc_extra_param->set_key(EXTRA_PARAM_KEY);
        grpc_extra_param->set_value(collection_schema.extra_params_.dump());
        LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
        SET_RESPONSE(response->mutable_status(), status, context);
    } catch (std::exception& ex) {
        Status status = Status{SERVER_UNEXPECTED_ERROR, "Parsing json string wrong"};
        SET_RESPONSE(response->mutable_status(), status, context);
    }
    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::CountCollection(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                    ::milvus::grpc::CollectionRowCount* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    int64_t row_count = 0;
    Status status = req_handler_.CountEntities(GetContext(context), request->collection_name(), row_count);
    response->set_collection_row_count(row_count);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::ShowCollections(::grpc::ServerContext* context, const ::milvus::grpc::Command* request,
                                    ::milvus::grpc::CollectionNameList* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::vector<std::string> collections;
    Status status = req_handler_.ListCollections(GetContext(context), collections);
    for (auto& collection : collections) {
        response->add_collection_names(collection);
    }

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::ShowCollectionInfo(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                       ::milvus::grpc::CollectionInfo* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::string collection_stats;
    Status status = req_handler_.GetCollectionStats(GetContext(context), request->collection_name(), collection_stats);
    response->set_json_info(collection_stats);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::Cmd(::grpc::ServerContext* context, const ::milvus::grpc::Command* request,
                        ::milvus::grpc::StringReply* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::string reply;
    Status status;

    std::string cmd = request->cmd();
    std::vector<std::string> requests;
    if (cmd == "requests") {
        std::lock_guard<std::mutex> lock(context_map_mutex_);
        for (auto& iter : context_map_) {
            if (nullptr == iter.second) {
                continue;
            }
            if (iter.second->ReqID() == get_request_id(context)) {
                continue;
            }
            auto request_str = RequestMap(iter.second->GetReqType()) + "-" + iter.second->ReqID();
            requests.emplace_back(request_str);
        }
        milvus::json reply_json;
        reply_json["requests"] = requests;
        reply = reply_json.dump();
        response->set_string_reply(reply);
    } else {
        status = req_handler_.Cmd(GetContext(context), cmd, reply);
        response->set_string_reply(reply);
    }

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::DeleteByID(::grpc::ServerContext* context, const ::milvus::grpc::DeleteByIDParam* request,
                               ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    // step 1: prepare id array
    engine::IDNumbers ids;
    for (int i = 0; i < request->id_array_size(); i++) {
        ids.push_back(request->id_array(i));
    }

    // step 2: delete vector
    Status status = req_handler_.DeleteEntityByID(GetContext(context), request->collection_name(), ids);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::PreloadCollection(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                      ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status = req_handler_.LoadCollection(GetContext(context), request->collection_name());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::CreatePartition(::grpc::ServerContext* context, const ::milvus::grpc::PartitionParam* request,
                                    ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status = req_handler_.CreatePartition(GetContext(context), request->collection_name(), request->tag());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::HasPartition(::grpc::ServerContext* context, const ::milvus::grpc::PartitionParam* request,
                                 ::milvus::grpc::BoolReply* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    bool has_collection = false;

    Status status =
        req_handler_.HasPartition(GetContext(context), request->collection_name(), request->tag(), has_collection);
    response->set_bool_reply(has_collection);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::ShowPartitions(::grpc::ServerContext* context, const ::milvus::grpc::CollectionName* request,
                                   ::milvus::grpc::PartitionList* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::vector<std::string> partition_names;
    Status status = req_handler_.ListPartitions(GetContext(context), request->collection_name(), partition_names);
    for (auto& pn : partition_names) {
        response->add_partition_tag_array(pn);
    }

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::DropPartition(::grpc::ServerContext* context, const ::milvus::grpc::PartitionParam* request,
                                  ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status = req_handler_.DropPartition(GetContext(context), request->collection_name(), request->tag());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::Flush(::grpc::ServerContext* context, const ::milvus::grpc::FlushParam* request,
                          ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    std::vector<std::string> collection_names;
    collection_names.reserve(collection_names.size());
    for (int32_t i = 0; i < request->collection_name_array().size(); i++) {
        collection_names.push_back(request->collection_name_array(i));
    }
    Status status = req_handler_.Flush(GetContext(context), collection_names);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

::grpc::Status
GrpcRequestHandler::Compact(::grpc::ServerContext* context, const ::milvus::grpc::CompactParam* request,
                            ::milvus::grpc::Status* response) {
    CHECK_NULLPTR_RETURN(request);
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status = req_handler_.Compact(GetContext(context), request->collection_name(), request->threshold());

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response, status, context);

    return ::grpc::Status::OK;
}

/*******************************************New Interface*********************************************/

::grpc::Status
GrpcRequestHandler::Insert(::grpc::ServerContext* context, const ::milvus::grpc::InsertParam* request,
                           ::milvus::grpc::EntityIds* response) {
    //    engine::VectorsData vectors;
    //    CopyRowRecords(request->entity().vector_data(0).value(), request->entity_id_array(), vectors);

    auto request_id = GetContext(context)->ReqID();
    CHECK_NULLPTR_RETURN(request);
    ScopedTimer scoped_timer([this](double lantency) { this->operation_insert_histogram_.Observe(lantency); });
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", request_id.c_str(), __func__);

    // By limiting the number of requests processed at the same time,
    // avoid excessive memory consumption (causing oom in extreme cases).
    // acquire resources
    int64_t request_size = request->ByteSizeLong();
    WaitToInsert(request_id, request_size);

    auto satus = OnInsert(context, request, response);

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", request_id.c_str(), __func__);

    // release resources
    FinishInsert(request_id, request_size);

    return satus;
}

::grpc::Status
GrpcRequestHandler::OnInsert(::grpc::ServerContext* context, const ::milvus::grpc::InsertParam* request,
                             ::milvus::grpc::EntityIds* response) {
    engine::IDNumbers vector_ids;
    vector_ids.reserve(request->entity_id_array_size());
    for (int64_t i = 0; i < request->entity_id_array_size(); i++) {
        if (request->entity_id_array(i) < 0) {
            auto status = Status{SERVER_INVALID_ROWRECORD_ARRAY, "id can not be negative number"};
            SET_RESPONSE(response->mutable_status(), status, context);
            return ::grpc::Status::OK;
        }
    }

    auto valid_row_count = [&](int32_t& base, int32_t test) -> bool {
        if (base < 0) {
            base = test;
            if (request->entity_id_array_size() > 0 && base != request->entity_id_array_size()) {
                auto status = Status{SERVER_INVALID_ROWRECORD_ARRAY, "ID size not matches entity size"};
                SET_RESPONSE(response->mutable_status(), status, context);
                return false;
            }
        } else if (base != test) {
            auto status = Status{SERVER_INVALID_ROWRECORD_ARRAY, "Field row count inconsist"};
            SET_RESPONSE(response->mutable_status(), status, context);
            return false;
        }
        return true;
    };

    // construct insert parameter
    InsertParam insert_param;
    int32_t row_num = -1;
    auto field_size = request->fields_size();
    for (int i = 0; i < field_size; i++) {
        auto grpc_int32_size = request->fields(i).attr_record().int32_value_size();
        auto grpc_int64_size = request->fields(i).attr_record().int64_value_size();
        auto grpc_float_size = request->fields(i).attr_record().float_value_size();
        auto grpc_double_size = request->fields(i).attr_record().double_value_size();
        const auto& field = request->fields(i);
        auto& field_name = field.field_name();

        if (grpc_int32_size > 0) {
            if (!valid_row_count(row_num, grpc_int32_size)) {
                return ::grpc::Status::OK;
            }
            RecordDataAddr<int32_t>(field_name, grpc_int32_size, field.attr_record().int32_value().data(),
                                    insert_param);
        } else if (grpc_int64_size > 0) {
            if (!valid_row_count(row_num, grpc_int64_size)) {
                return ::grpc::Status::OK;
            }
            RecordDataAddr<int64_t>(field_name, grpc_int64_size, field.attr_record().int64_value().data(),
                                    insert_param);
        } else if (grpc_float_size > 0) {
            if (!valid_row_count(row_num, grpc_float_size)) {
                return ::grpc::Status::OK;
            }
            RecordDataAddr<float>(field_name, grpc_float_size, field.attr_record().float_value().data(), insert_param);
        } else if (grpc_double_size > 0) {
            if (!valid_row_count(row_num, grpc_double_size)) {
                return ::grpc::Status::OK;
            }
            RecordDataAddr<double>(field_name, grpc_double_size, field.attr_record().double_value().data(),
                                   insert_param);
        } else {
            if (!valid_row_count(row_num, field.vector_record().records_size())) {
                return ::grpc::Status::OK;
            }
            RecordVectorDataAddr(field_name, field.vector_record().records(), insert_param);
        }
    }
    insert_param.row_count_ = row_num;

    // copy id array
    if (request->entity_id_array_size() > 0) {
        RecordDataAddr<int64_t>(engine::FIELD_UID, request->entity_id_array_size(), request->entity_id_array().data(),
                                insert_param);
    }

    std::string collection_name = request->collection_name();
    std::string partition_name = request->partition_tag();
    Status status = req_handler_.Insert(GetContext(context), collection_name, partition_name, insert_param);
    if (!status.ok()) {
        SET_RESPONSE(response->mutable_status(), status, context);
        return ::grpc::Status::OK;
    }

    // return generated ids
    if (!insert_param.id_returned_.empty()) {
        response->mutable_entity_id_array()->Resize(static_cast<int>(insert_param.id_returned_.size()), 0);
        memcpy(response->mutable_entity_id_array()->mutable_data(), insert_param.id_returned_.data(),
               insert_param.id_returned_.size() * sizeof(int64_t));
    }

    SET_RESPONSE(response->mutable_status(), status, context);
    return ::grpc::Status::OK;
}

#if 0
Status
ParseTermQuery(const milvus::json& term_json, std::unordered_map<std::string, engine::DataType> field_type,
               query::TermQueryPtr& term_query) {
    std::string field_name = term_json["field"].get<std::string>();
    auto term_value_json = term_json["values"];
    if (!term_value_json.is_array()) {
        std::string msg = "Term json string is not an array";
        return Status{SERVER_INVALID_DSL_PARAMETER, msg};
    }

    auto term_size = term_value_json.size();
    term_query->field_name = field_name;
    term_query->field_value.resize(term_size * sizeof(int64_t));

    switch (field_type.at(field_name)) {
        case engine::DataType::INT8: {
            std::vector<int64_t> term_value(term_size, 0);
            for (uint64_t i = 0; i < term_size; i++) {
                term_value[i] = term_value_json[i].get<int8_t>();
            }
            memcpy(term_query->field_value.data(), term_value.data(), term_size * sizeof(int64_t));
            break;
        }
        case engine::DataType::INT16: {
            std::vector<int64_t> term_value(term_size, 0);
            for (uint64_t i = 0; i < term_size; i++) {
                term_value[i] = term_value_json[i].get<int16_t>();
            }
            memcpy(term_query->field_value.data(), term_value.data(), term_size * sizeof(int64_t));
            break;
        }
        case engine::DataType::INT32: {
            std::vector<int64_t> term_value(term_size, 0);
            for (uint64_t i = 0; i < term_size; i++) {
                term_value[i] = term_value_json[i].get<int32_t>();
            }
            memcpy(term_query->field_value.data(), term_value.data(), term_size * sizeof(int64_t));
            break;
        }
        case engine::DataType::INT64: {
            std::vector<int64_t> term_value(term_size, 0);
            for (uint64_t i = 0; i < term_size; ++i) {
                term_value[i] = term_value_json[i].get<int64_t>();
            }
            memcpy(term_query->field_value.data(), term_value.data(), term_size * sizeof(int64_t));
            break;
        }
        case engine::DataType::FLOAT: {
            std::vector<double> term_value(term_size, 0);
            for (uint64_t i = 0; i < term_size; ++i) {
                term_value[i] = term_value_json[i].get<float>();
            }
            memcpy(term_query->field_value.data(), term_value.data(), term_size * sizeof(double));
            break;
        }
        case engine::DataType::DOUBLE: {
            std::vector<double> term_value(term_size, 0);
            for (uint64_t i = 0; i < term_size; ++i) {
                term_value[i] = term_value_json[i].get<double>();
            }
            memcpy(term_query->field_value.data(), term_value.data(), term_size * sizeof(double));
            break;
        }
    }
    return Status::OK();
}

Status
ParseRangeQuery(const milvus::json& range_json, query::RangeQueryPtr& range_query) {
    std::string field_name = range_json["field"];
    range_query->field_name = field_name;

    auto range_value_json = range_json["values"];
    if (range_value_json.contains("lt")) {
        query::CompareExpr compare_expr;
        compare_expr.compare_operator = query::CompareOperator::LT;
        compare_expr.operand = range_value_json["lt"].get<std::string>();
        range_query->compare_expr.emplace_back(compare_expr);
    }
    if (range_value_json.contains("lte")) {
        query::CompareExpr compare_expr;
        compare_expr.compare_operator = query::CompareOperator::LTE;
        compare_expr.operand = range_value_json["lte"].get<std::string>();
        range_query->compare_expr.emplace_back(compare_expr);
    }
    if (range_value_json.contains("eq")) {
        query::CompareExpr compare_expr;
        compare_expr.compare_operator = query::CompareOperator::EQ;
        compare_expr.operand = range_value_json["eq"].get<std::string>();
        range_query->compare_expr.emplace_back(compare_expr);
    }
    if (range_value_json.contains("ne")) {
        query::CompareExpr compare_expr;
        compare_expr.compare_operator = query::CompareOperator::NE;
        compare_expr.operand = range_value_json["ne"].get<std::string>();
        range_query->compare_expr.emplace_back(compare_expr);
    }
    if (range_value_json.contains("gt")) {
        query::CompareExpr compare_expr;
        compare_expr.compare_operator = query::CompareOperator::GT;
        compare_expr.operand = range_value_json["gt"].get<std::string>();
        range_query->compare_expr.emplace_back(compare_expr);
    }
    if (range_value_json.contains("gte")) {
        query::CompareExpr compare_expr;
        compare_expr.compare_operator = query::CompareOperator::GTE;
        compare_expr.operand = range_value_json["gte"].get<std::string>();
        range_query->compare_expr.emplace_back(compare_expr);
    }
    return Status::OK();
}
#endif

Status
GrpcRequestHandler::ProcessLeafQueryJson(const milvus::json& query_json, query::BooleanQueryPtr& query,
                                         std::string& field_name) {
    if (query_json.contains("term")) {
        auto leaf_query = std::make_shared<query::LeafQuery>();
        auto term_query = std::make_shared<query::TermQuery>();
        milvus::json json_obj = query_json["term"];
        JSON_NULL_CHECK(json_obj);
        JSON_OBJECT_CHECK(json_obj);
        term_query->json_obj = json_obj;
        milvus::json::iterator json_it = json_obj.begin();
        field_name = json_it.key();

        leaf_query->term_query = term_query;
        query->AddLeafQuery(leaf_query);
    } else if (query_json.contains("range")) {
        auto leaf_query = std::make_shared<query::LeafQuery>();
        auto range_query = std::make_shared<query::RangeQuery>();
        milvus::json json_obj = query_json["range"];
        JSON_NULL_CHECK(json_obj);
        JSON_OBJECT_CHECK(json_obj);
        range_query->json_obj = json_obj;
        milvus::json::iterator json_it = json_obj.begin();
        field_name = json_it.key();

        leaf_query->range_query = range_query;
        query->AddLeafQuery(leaf_query);
    } else if (query_json.contains("vector")) {
        auto leaf_query = std::make_shared<query::LeafQuery>();
        auto vector_json = query_json["vector"];
        JSON_NULL_CHECK(vector_json);

        leaf_query->vector_placeholder = vector_json.get<std::string>();
        query->AddLeafQuery(leaf_query);
    } else {
        return Status{SERVER_INVALID_ARGUMENT, "Leaf query get wrong key"};
    }
    return Status::OK();
}

Status
GrpcRequestHandler::ProcessBooleanQueryJson(const milvus::json& query_json, query::BooleanQueryPtr& boolean_query,
                                            query::QueryPtr& query_ptr) {
    if (query_json.empty()) {
        return Status{SERVER_INVALID_ARGUMENT, "BoolQuery is null"};
    }
    for (auto& el : query_json.items()) {
        if (el.key() == "must") {
            boolean_query->SetOccur(query::Occur::MUST);
            auto must_json = el.value();
            if (!must_json.is_array()) {
                std::string msg = "Must json string is not an array";
                return Status{SERVER_INVALID_DSL_PARAMETER, msg};
            }

            for (auto& json : must_json) {
                auto must_query = std::make_shared<query::BooleanQuery>();
                if (json.contains("must") || json.contains("should") || json.contains("must_not")) {
                    STATUS_CHECK(ProcessBooleanQueryJson(json, must_query, query_ptr));
                    boolean_query->AddBooleanQuery(must_query);
                } else {
                    std::string field_name;
                    STATUS_CHECK(ProcessLeafQueryJson(json, boolean_query, field_name));
                    if (!field_name.empty()) {
                        query_ptr->index_fields.insert(field_name);
                    }
                }
            }
        } else if (el.key() == "should") {
            boolean_query->SetOccur(query::Occur::SHOULD);
            auto should_json = el.value();
            if (!should_json.is_array()) {
                std::string msg = "Should json string is not an array";
                return Status{SERVER_INVALID_DSL_PARAMETER, msg};
            }

            for (auto& json : should_json) {
                auto should_query = std::make_shared<query::BooleanQuery>();
                if (json.contains("must") || json.contains("should") || json.contains("must_not")) {
                    STATUS_CHECK(ProcessBooleanQueryJson(json, should_query, query_ptr));
                    boolean_query->AddBooleanQuery(should_query);
                } else {
                    std::string field_name;
                    STATUS_CHECK(ProcessLeafQueryJson(json, boolean_query, field_name));
                    if (!field_name.empty()) {
                        query_ptr->index_fields.insert(field_name);
                    }
                }
            }
        } else if (el.key() == "must_not") {
            boolean_query->SetOccur(query::Occur::MUST_NOT);
            auto should_json = el.value();
            if (!should_json.is_array()) {
                std::string msg = "Must_not json string is not an array";
                return Status{SERVER_INVALID_DSL_PARAMETER, msg};
            }

            for (auto& json : should_json) {
                if (json.contains("must") || json.contains("should") || json.contains("must_not")) {
                    auto must_not_query = std::make_shared<query::BooleanQuery>();
                    STATUS_CHECK(ProcessBooleanQueryJson(json, must_not_query, query_ptr));
                    boolean_query->AddBooleanQuery(must_not_query);
                } else {
                    std::string field_name;
                    STATUS_CHECK(ProcessLeafQueryJson(json, boolean_query, field_name));
                    if (!field_name.empty()) {
                        query_ptr->index_fields.insert(field_name);
                    }
                }
            }
        } else {
            std::string msg = "BoolQuery json string does not include bool query";
            return Status{SERVER_INVALID_DSL_PARAMETER, msg};
        }
    }

    return Status::OK();
}

Status
GrpcRequestHandler::DeserializeDslToBoolQuery(
    const google::protobuf::RepeatedPtrField<::milvus::grpc::VectorParam>& vector_params, const std::string& dsl_string,
    query::BooleanQueryPtr& boolean_query, query::QueryPtr& query_ptr) {
    try {
        milvus::json dsl_json = json::parse(dsl_string);

        if (dsl_json.empty()) {
            return Status{SERVER_INVALID_ARGUMENT, "Query dsl is null"};
        }
        auto status = Status::OK();
        if (vector_params.size() != 1) {
            return Status(SERVER_INVALID_DSL_PARAMETER, "There should only be one vector query");
        }
        for (const auto& vector_param : vector_params) {
            const std::string& vector_string = vector_param.json();
            milvus::json vector_json = json::parse(vector_string);
            milvus::json::iterator it = vector_json.begin();
            std::string placeholder = it.key();

            auto vector_query = std::make_shared<query::VectorQuery>();
            milvus::json::iterator vector_param_it = it.value().begin();
            if (vector_param_it != it.value().end()) {
                const std::string& field_name = vector_param_it.key();
                vector_query->field_name = field_name;
                milvus::json param_json = vector_param_it.value();
                int64_t topk = param_json["topk"];
                STATUS_CHECK(server::ValidateSearchTopk(topk));
                vector_query->topk = topk;
                if (param_json.contains("metric_type")) {
                    std::string metric_type = param_json["metric_type"];
                    vector_query->metric_type = metric_type;
                    query_ptr->metric_types.insert({field_name, param_json["metric_type"]});
                }
                if (!vector_param_it.value()["params"].empty()) {
                    vector_query->extra_params = vector_param_it.value()["params"];
                }
                query_ptr->index_fields.insert(field_name);
            }

            engine::VectorsData vector_data;
            CopyRowRecords(vector_param.row_record().records(),
                           google::protobuf::RepeatedField<google::protobuf::int64>(), vector_data);
            vector_query->query_vector.vector_count = vector_data.vector_count_;
            vector_query->query_vector.binary_data.swap(vector_data.binary_data_);
            vector_query->query_vector.float_data.swap(vector_data.float_data_);

            query_ptr->vectors.insert(std::make_pair(placeholder, vector_query));
        }
        if (dsl_json.contains("bool")) {
            auto boolean_query_json = dsl_json["bool"];
            JSON_NULL_CHECK(boolean_query_json);
            STATUS_CHECK(ProcessBooleanQueryJson(boolean_query_json, boolean_query, query_ptr));
        } else {
            return Status(SERVER_INVALID_DSL_PARAMETER, "DSL does not include bool query");
        }
        return Status::OK();
    } catch (std::exception& e) {
        return Status(SERVER_INVALID_DSL_PARAMETER, e.what());
    }
}

::grpc::Status
GrpcRequestHandler::Search(::grpc::ServerContext* context, const ::milvus::grpc::SearchParam* request,
                           ::milvus::grpc::QueryResult* response) {
    CHECK_NULLPTR_RETURN(request);
    ScopedTimer scoped_timer([this](double lantency) { this->operation_search_histogram_.Observe(lantency); });
    LOG_SERVER_INFO_ << LogOut("Request [%s] %s begin.", GetContext(context)->ReqID().c_str(), __func__);

    Status status;

    CollectionSchema collection_schema;
    status = req_handler_.GetCollectionInfo(GetContext(context), request->collection_name(), collection_schema);

    auto grpc_entity = response->mutable_entities();
    if (!status.ok()) {
        SET_RESPONSE(response->mutable_status(), status, context);
        return ::grpc::Status::OK;
    }

    query::BooleanQueryPtr boolean_query = std::make_shared<query::BooleanQuery>();
    query::QueryPtr query_ptr = std::make_shared<query::Query>();
    query_ptr->collection_id = request->collection_name();

    status = DeserializeDslToBoolQuery(request->vector_param(), request->dsl(), boolean_query, query_ptr);
    if (!status.ok()) {
        SET_RESPONSE(response->mutable_status(), status, context);
        return ::grpc::Status::OK;
    }

    status = query::QueryUtil::ValidateBooleanQuery(boolean_query);
    if (!status.ok()) {
        SET_RESPONSE(response->mutable_status(), status, context);
        return ::grpc::Status::OK;
    }

    query::GeneralQueryPtr general_query = std::make_shared<query::GeneralQuery>();
    query::QueryUtil::GenBinaryQuery(boolean_query, general_query->bin);
    query_ptr->root = general_query;

    if (!query::QueryUtil::ValidateBinaryQuery(general_query->bin)) {
        status = Status{SERVER_INVALID_BINARY_QUERY, "Generate wrong binary query tree"};
        SET_RESPONSE(grpc_entity->mutable_status(), status, context);
        return ::grpc::Status::OK;
    }

    std::vector<std::string> partition_list;
    partition_list.resize(request->partition_tag_array_size());
    for (int i = 0; i < request->partition_tag_array_size(); ++i) {
        partition_list[i] = request->partition_tag_array(i);
    }

    query_ptr->partitions = partition_list;

    milvus::json json_params;
    for (int i = 0; i < request->extra_params_size(); i++) {
        const ::milvus::grpc::KeyValuePair& extra = request->extra_params(i);
        if (extra.key() == EXTRA_PARAM_KEY) {
            json_params = json::parse(extra.value());
        }
    }

    engine::QueryResultPtr result = std::make_shared<engine::QueryResult>();
    engine::snapshot::FieldElementMappings field_mappings;

    status = req_handler_.Search(GetContext(context), query_ptr, json_params, field_mappings, result);

    if (!status.ok()) {
        SET_RESPONSE(response->mutable_status(), status, context);
        return ::grpc::Status::OK;
    }

    // step 6: construct and return result
    response->set_row_num(result->row_num_);
    int64_t id_size = result->result_ids_.size();
    for (auto result_id : result->result_ids_) {
        if (result_id == -1) {
            id_size--;
            grpc_entity->add_valid_row(false);
        } else {
            grpc_entity->add_valid_row(true);
        }
    }

    CopyDataChunkToEntity(result->data_chunk_, field_mappings, id_size, grpc_entity);

    grpc_entity->mutable_ids()->Resize(static_cast<int>(result->result_ids_.size()), 0);
    memcpy(grpc_entity->mutable_ids()->mutable_data(), result->result_ids_.data(),
           result->result_ids_.size() * sizeof(int64_t));

    response->mutable_distances()->Resize(static_cast<int>(result->result_distances_.size()), 0.0);
    memcpy(response->mutable_distances()->mutable_data(), result->result_distances_.data(),
           result->result_distances_.size() * sizeof(float));

    LOG_SERVER_INFO_ << LogOut("Request [%s] %s end.", GetContext(context)->ReqID().c_str(), __func__);
    SET_RESPONSE(response->mutable_status(), status, context);

    return ::grpc::Status::OK;
}

void
GrpcRequestHandler::WaitToInsert(const std::string& request_id, int64_t request_size) {
    std::unique_lock<std::mutex> lock(max_concurrent_insert_request_mutex);
    insert_event_cv_.wait(lock, [&] { return max_concurrent_insert_request_size - request_size > 0; });
    max_concurrent_insert_request_size -= request_size;
    LOG_SERVER_DEBUG_ << LogOut(
        "Start to process insert request [%s], "
        "gRPC buffer size(request/remain/total): %s, %s, %s",
        request_id.c_str(), CommonUtil::ConvertSize(request_size).c_str(),
        CommonUtil::ConvertSize(max_concurrent_insert_request_size).c_str(),
        CommonUtil::ConvertSize(max_concurrent_insert_request_size_).c_str());
    lock.unlock();
}

void
GrpcRequestHandler::FinishInsert(const std::string& request_id, int64_t request_size) {
    {
        std::lock_guard<std::mutex> lock(max_concurrent_insert_request_mutex);
        max_concurrent_insert_request_size += request_size;
        LOG_SERVER_DEBUG_ << LogOut(
            "Finish to process insert request [%s], "
            "gRPC buffer size(request/remain/total): %s, %s, %s",
            request_id.c_str(), CommonUtil::ConvertSize(request_size).c_str(),
            CommonUtil::ConvertSize(max_concurrent_insert_request_size).c_str(),
            CommonUtil::ConvertSize(max_concurrent_insert_request_size_).c_str());
    }
    insert_event_cv_.notify_all();
}

}  // namespace grpc
}  // namespace server
}  // namespace milvus
