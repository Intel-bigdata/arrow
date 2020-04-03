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

#include <arrow/dataset/file_base.h>
#include <arrow/dataset/api.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/ipc/api.h>
#include <arrow/util/iterator.h>
#include <arrow/filesystem/hdfs.h>
#include <arrow/io/api.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/message.h>
#include "concurrent_map.h"
#include "arrow/compute/kernel.h"
#include "arrow/compute/kernels/cast.h"
#include "arrow/compute/kernels/compare.h"
#include "jni/dataset/Types.pb.h"

#include "org_apache_arrow_dataset_jni_JniWrapper.h"
#include "org_apache_arrow_dataset_file_JniWrapper.h"

static jclass illegal_access_exception_class;
static jclass illegal_argument_exception_class;
static jclass runtime_exception_class;

static jclass record_batch_handle_class;
static jclass record_batch_handle_field_class;
static jclass record_batch_handle_buffer_class;

static jmethodID record_batch_handle_constructor;
static jmethodID record_batch_handle_field_constructor;
static jmethodID record_batch_handle_buffer_constructor;

static jint JNI_VERSION = JNI_VERSION_1_6;

using arrow::jni::ConcurrentMap;

static ConcurrentMap<std::shared_ptr<arrow::dataset::DatasetFactory>> dataset_factory_holder_;
static ConcurrentMap<std::shared_ptr<arrow::dataset::Dataset>> dataset_holder_;
static ConcurrentMap<std::shared_ptr<arrow::dataset::ScanTask>> scan_task_holder_;
static ConcurrentMap<std::shared_ptr<arrow::dataset::Scanner>> scanner_holder_;
static ConcurrentMap<std::shared_ptr<arrow::RecordBatchIterator>> iterator_holder_;
static ConcurrentMap<std::shared_ptr<arrow::Buffer>> buffer_holder_;

#define JNI_ASSIGN_OR_THROW_NAME(x, y) ARROW_CONCAT(x, y)

#define JNI_ASSIGN_OR_THROW_IMPL(t, lhs, rexpr)                             \
  auto t = (rexpr);                                                         \
  if (!t.status().ok()) {                                                   \
    env->ThrowNew(runtime_exception_class, t.status().message().c_str());   \
  }                                                                         \
  lhs = std::move(t).ValueOrDie();

#define JNI_ASSIGN_OR_THROW(lhs, rexpr)                                     \
  JNI_ASSIGN_OR_THROW_IMPL(JNI_ASSIGN_OR_THROW_NAME(_tmp_var, __COUNTER__), lhs, rexpr)

#define JNI_ASSERT_OK_OR_THROW(expr)                                          \
  do {                                                                        \
    auto _res = (expr);                                                       \
    arrow::Status _st = ::arrow::internal::GenericToStatus(_res);             \
    if (!_st.ok()) {                                                          \
       env->ThrowNew(runtime_exception_class, _st.message().c_str());  \
    }                                                                         \
  } while (false);

jclass CreateGlobalClassReference(JNIEnv* env, const char* class_name) {
  jclass local_class = env->FindClass(class_name);
  jclass global_class = (jclass)env->NewGlobalRef(local_class);
  env->DeleteLocalRef(local_class);
  return global_class;
}

jmethodID GetMethodID(JNIEnv* env, jclass this_class, const char* name, const char* sig) {
  jmethodID ret = env->GetMethodID(this_class, name, sig);
  if (ret == nullptr) {
    std::string error_message = "Unable to find method " + std::string(name) +
        " within signature" + std::string(sig);
    env->ThrowNew(illegal_access_exception_class, error_message.c_str());
  }
  return ret;
}

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }

  illegal_access_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/IllegalAccessException;");
  illegal_argument_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/IllegalArgumentException;");
  runtime_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/RuntimeException;");

  record_batch_handle_class =
      CreateGlobalClassReference(env, "Lorg/apache/arrow/dataset/jni/NativeRecordBatchHandle;");
  record_batch_handle_field_class =
      CreateGlobalClassReference(env, "Lorg/apache/arrow/dataset/jni/NativeRecordBatchHandle$Field;");
  record_batch_handle_buffer_class =
      CreateGlobalClassReference(env, "Lorg/apache/arrow/dataset/jni/NativeRecordBatchHandle$Buffer;");

  record_batch_handle_constructor = GetMethodID(env, record_batch_handle_class, "<init>",
                                                "(J[Lorg/apache/arrow/dataset/jni/NativeRecordBatchHandle$Field;"
                                                "[Lorg/apache/arrow/dataset/jni/NativeRecordBatchHandle$Buffer;)V");
  record_batch_handle_field_constructor = GetMethodID(env, record_batch_handle_field_class, "<init>",
                                                      "(JJ)V");
  record_batch_handle_buffer_constructor = GetMethodID(env, record_batch_handle_buffer_class, "<init>",
                                                       "(JJJJ)V");

  env->ExceptionDescribe();

  return JNI_VERSION;
}


void JNI_OnUnload(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION);
  env->DeleteGlobalRef(illegal_access_exception_class);
  env->DeleteGlobalRef(illegal_argument_exception_class);
  env->DeleteGlobalRef(runtime_exception_class);
  env->DeleteGlobalRef(record_batch_handle_class);
  env->DeleteGlobalRef(record_batch_handle_field_class);
  env->DeleteGlobalRef(record_batch_handle_buffer_class);

  dataset_factory_holder_.Clear();
  dataset_holder_.Clear();
  scan_task_holder_.Clear();
  scanner_holder_.Clear();
  iterator_holder_.Clear();
  buffer_holder_.Clear();
}

std::shared_ptr<arrow::Schema> SchemaFromColumnNames(
    const std::shared_ptr<arrow::Schema>& input, const std::vector<std::string>& column_names) {
  std::vector<std::shared_ptr<arrow::Field>> columns;
  for (const auto& name : column_names) {
    columns.push_back(input->GetFieldByName(name));
  }
  return std::make_shared<arrow::Schema>(columns);
}

std::shared_ptr<arrow::dataset::FileFormat> GetFileFormat(JNIEnv *env, jint id) {
  switch (id) {
    case 0:
      return std::make_shared<arrow::dataset::ParquetFileFormat>();
    default:
      std::string error_message = "illegal file format id: " + std::to_string(id);
      env->ThrowNew(illegal_argument_exception_class, error_message.c_str());
      return nullptr; // unreachable
  }
}

std::shared_ptr<arrow::fs::FileSystem> GetFileSystem(JNIEnv *env, jint id, std::string path,
                                       std::string* out_path) {
  switch (id) {
    case 0:
      *out_path = path;
      return std::make_shared<arrow::fs::LocalFileSystem>();
    case 1: {
      JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::fs::FileSystem> ret, arrow::fs::FileSystemFromUri(path, out_path))
      return ret;
    }
    default:std::string error_message = "illegal filesystem id: " + std::to_string(id);
      env->ThrowNew(illegal_argument_exception_class, error_message.c_str());
      return nullptr; // unreachable
  }
}

std::string JStringToCString(JNIEnv* env, jstring string) {
  jboolean copied;
  int32_t length = env->GetStringUTFLength(string);
  const char *chars = env->GetStringUTFChars(string, &copied);
  std::string str = std::string(chars, length);
  // fixme calling ReleaseStringUTFChars if memory leak faced
  return str;
}

std::vector<std::string> ToStringVector(JNIEnv* env, jobjectArray& str_array) {
  int length = env->GetArrayLength(str_array);
  std::vector<std::string> vector;
  for (int i = 0; i < length; i++) {
    auto string = (jstring) (env->GetObjectArrayElement(str_array, i));
    vector.push_back(JStringToCString(env, string));
  }
  return vector;
}

template <typename T>
std::vector<T> collect(JNIEnv* env, arrow::Iterator<T> itr) {
  std::vector<T> vector;
  while(true) {
    JNI_ASSIGN_OR_THROW(T t, itr.Next())
    if (!t) {
      break;
    }
    vector.push_back(t);
  }
  return vector;
}

// FIXME: COPIED FROM intel/master on which this branch is not rebased yet
// FIXME: https://github.com/Intel-bigdata/arrow/blob/02502a4eb59834c2471dd629e77dbeed19559f68/cpp/src/jni/jni_common.h#L239-L254
jbyteArray ToSchemaByteArray(JNIEnv* env, std::shared_ptr<arrow::Schema> schema) {
  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::Buffer> buffer,
      arrow::ipc::SerializeSchema(*schema.get(), nullptr, arrow::default_memory_pool()));

  jbyteArray out = env->NewByteArray(buffer->size());
  auto src = reinterpret_cast<const jbyte*>(buffer->data());
  env->SetByteArrayRegion(out, 0, buffer->size(), src);
  return out;
}

// FIXME: COPIED FROM intel/master on which this branch is not rebased yet
// FIXME: https://github.com/Intel-bigdata/arrow/blob/02502a4eb59834c2471dd629e77dbeed19559f68/cpp/src/jni/jni_common.h#L256-L272
std::shared_ptr<arrow::Schema> FromSchemaByteArray(JNIEnv* env, jbyteArray schemaBytes) {
  arrow::ipc::DictionaryMemo in_memo;
  int schemaBytes_len = env->GetArrayLength(schemaBytes);
  jbyte* schemaBytes_data = env->GetByteArrayElements(schemaBytes, 0);
  auto serialized_schema =
      std::make_shared<arrow::Buffer>((uint8_t*)schemaBytes_data, schemaBytes_len);
  arrow::io::BufferReader buf_reader(serialized_schema);
  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::Schema> schema, arrow::ipc::ReadSchema(&buf_reader, &in_memo))
  env->ReleaseByteArrayElements(schemaBytes, schemaBytes_data, JNI_ABORT);
  return schema;
}

bool ParseProtobuf(uint8_t* buf, int bufLen, google::protobuf::Message* msg) {
    google::protobuf::io::CodedInputStream cis(buf, bufLen);
    cis.SetRecursionLimit(1000);
    return msg->ParseFromCodedStream(&cis);
}

void releaseFilterInput(jbyteArray condition_arr, jbyte* condition_bytes, JNIEnv* env) {
  env->ReleaseByteArrayElements(condition_arr, condition_bytes, JNI_ABORT);
}

// fixme in development. Not all node types considered.
std::shared_ptr<arrow::dataset::Expression> translateNode(types::TreeNode node, JNIEnv* env) {
  if (node.has_fieldnode()) {
    const types::FieldNode& f_node = node.fieldnode();
    const std::string& name = f_node.name();
    return std::make_shared<arrow::dataset::FieldExpression>(name);
  }
  if (node.has_intnode()) {
    const types::IntNode& int_node = node.intnode();
    int32_t val = int_node.value();
    return std::make_shared<arrow::dataset::ScalarExpression>(std::make_shared<arrow::Int32Scalar>(val));
  }
  if (node.has_longnode()) {
    const types::LongNode& long_node = node.longnode();
    int64_t val = long_node.value();
    return std::make_shared<arrow::dataset::ScalarExpression>(std::make_shared<arrow::Int64Scalar>(val));
  }
  if (node.has_floatnode()) {
    const types::FloatNode& float_node = node.floatnode();
    float_t val = float_node.value();
    return std::make_shared<arrow::dataset::ScalarExpression>(std::make_shared<arrow::FloatScalar>(val));
  }
  if (node.has_doublenode()) {
    const types::DoubleNode& double_node = node.doublenode();
    double_t val = double_node.value();
    return std::make_shared<arrow::dataset::ScalarExpression>(std::make_shared<arrow::DoubleScalar>(val));
  }
  if (node.has_booleannode()) {
    const types::BooleanNode& boolean_node = node.booleannode();
    bool val = boolean_node.value();
    return std::make_shared<arrow::dataset::ScalarExpression>(std::make_shared<arrow::BooleanScalar>(val));
  }
  if (node.has_andnode()) {
    const types::AndNode& and_node = node.andnode();
    const types::TreeNode& left_arg = and_node.leftarg();
    const types::TreeNode& right_arg = and_node.rightarg();
    return std::make_shared<arrow::dataset::AndExpression>(translateNode(left_arg, env), translateNode(right_arg, env));
  }
  if (node.has_ornode()) {
    const types::OrNode& or_node = node.ornode();
    const types::TreeNode& left_arg = or_node.leftarg();
    const types::TreeNode& right_arg = or_node.rightarg();
    return std::make_shared<arrow::dataset::OrExpression>(translateNode(left_arg, env), translateNode(right_arg, env));
  }
  if (node.has_cpnode()) {
    const types::ComparisonNode& cp_node = node.cpnode();
    const std::string& op_name = cp_node.opname();
    arrow::compute::CompareOperator op;
    if (op_name == "equal") {
      op = arrow::compute::CompareOperator::EQUAL;
    } else if (op_name == "greaterThan") {
      op = arrow::compute::CompareOperator::GREATER;
    } else if (op_name == "greaterThanOrEqual") {
      op = arrow::compute::CompareOperator::GREATER_EQUAL;
    } else if (op_name == "lessThan") {
      op = arrow::compute::CompareOperator::LESS;
    } else if (op_name == "lessThanOrEqual") {
      op = arrow::compute::CompareOperator::LESS_EQUAL;
    } else {
      std::string error_message = "Unknown operation name in comparison node";
      env->ThrowNew(illegal_argument_exception_class, error_message.c_str());
      return nullptr; // unreachable
    }
    const types::TreeNode& left_arg = cp_node.leftarg();
    const types::TreeNode& right_arg = cp_node.rightarg();
    return std::make_shared<arrow::dataset::ComparisonExpression>(op,
        translateNode(left_arg, env), translateNode(right_arg, env));
  }
  if (node.has_notnode()) {
    const types::NotNode& not_node = node.notnode();
    const ::types::TreeNode& child = not_node.args();
    std::shared_ptr<arrow::dataset::Expression> translatedChild = translateNode(child, env);
    return std::make_shared<arrow::dataset::NotExpression>(translatedChild);
  }
  if (node.has_isvalidnode()) {
    const types::IsValidNode& is_valid_node = node.isvalidnode();
    const ::types::TreeNode& child = is_valid_node.args();
    std::shared_ptr<arrow::dataset::Expression> translatedChild = translateNode(child, env);
    return std::make_shared<arrow::dataset::IsValidExpression>(translatedChild);
  }
  std::string error_message = "Unknown node type";
  env->ThrowNew(illegal_argument_exception_class, error_message.c_str());
  return nullptr; // unreachable
}

std::shared_ptr<arrow::dataset::Expression> translateFilter(types::Condition condition, JNIEnv* env) {
  const types::TreeNode& tree_node = condition.root();
  return translateNode(tree_node, env);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeDatasetFactory
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeDatasetFactory
    (JNIEnv *, jobject, jlong id) {
  dataset_factory_holder_.Erase(id);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    inspectSchema
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_inspectSchema
    (JNIEnv* env, jobject, jlong dataset_factor_id) {
  std::shared_ptr<arrow::dataset::DatasetFactory> d
      = dataset_factory_holder_.Lookup(dataset_factor_id);
  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::Schema> schema, d->Inspect())
  return ToSchemaByteArray(env, schema);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    createDataset
 * Signature: (J[B)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_createDataset
    (JNIEnv* env, jobject, jlong dataset_factory_id, jbyteArray schema_bytes) {
  std::shared_ptr<arrow::dataset::DatasetFactory> d
      = dataset_factory_holder_.Lookup(dataset_factory_id);
  std::shared_ptr<arrow::Schema> schema;
  schema = FromSchemaByteArray(env, schema_bytes);
  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::dataset::Dataset> dataset, d->Finish(schema))
  return dataset_holder_.Insert(dataset);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeDataset
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeDataset
    (JNIEnv *, jobject, jlong id) {
  dataset_holder_.Erase(id);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    createScanner
 * Signature: (J[Ljava/lang/String;[BJ)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_createScanner
    (JNIEnv* env, jobject, jlong dataset_id, jobjectArray columns,
        jbyteArray filter, jlong batch_size) {
  std::shared_ptr<arrow::dataset::ScanContext> context = std::make_shared<arrow::dataset::ScanContext>();
  std::shared_ptr<arrow::dataset::Dataset> dataset = dataset_holder_.Lookup(dataset_id);
  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::dataset::ScannerBuilder> scanner_builder, dataset->NewScan())

  std::vector<std::string> column_vector = ToStringVector(env, columns);
  JNI_ASSERT_OK_OR_THROW(scanner_builder->Project(column_vector));
  JNI_ASSERT_OK_OR_THROW(scanner_builder->BatchSize(batch_size));

  // initialize filters
  jsize exprs_len = env->GetArrayLength(filter);
  jbyte* exprs_bytes = env->GetByteArrayElements(filter, 0);
  types::Condition condition;
  if (!ParseProtobuf(reinterpret_cast<uint8_t*>(exprs_bytes), exprs_len, &condition)) {
    releaseFilterInput(filter, exprs_bytes, env);
    std::string error_message = "bad protobuf message";
    env->ThrowNew(illegal_argument_exception_class, error_message.c_str());
  }
  if (condition.has_root()) {
    JNI_ASSERT_OK_OR_THROW(scanner_builder->Filter(translateFilter(condition, env)));
  }
  JNI_ASSIGN_OR_THROW(auto scanner, scanner_builder->Finish())
  jlong id = scanner_holder_.Insert(scanner);
  releaseFilterInput(filter, exprs_bytes, env);
  return id;
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    getSchemaFromScanner
 * Signature: (J)[B
 */
JNIEXPORT jbyteArray JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_getSchemaFromScanner
    (JNIEnv* env, jobject, jlong scanner_id) {
  std::shared_ptr<arrow::Schema> schema = scanner_holder_.Lookup(scanner_id)->schema();
  return ToSchemaByteArray(env, schema);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    getScanTasksFromScanner
 * Signature: (J)[J
 */
JNIEXPORT jlongArray JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_getScanTasksFromScanner
    (JNIEnv* env, jobject, jlong scanner_id) {
  std::shared_ptr<arrow::dataset::Scanner> scanner = scanner_holder_.Lookup(scanner_id);
  JNI_ASSIGN_OR_THROW(arrow::dataset::ScanTaskIterator itr, scanner->Scan())
  std::vector<std::shared_ptr<arrow::dataset::ScanTask>> vector = collect(env, std::move(itr));
  jlongArray ret = env->NewLongArray(vector.size());
  for (unsigned long i = 0; i < vector.size(); i++) {
    std::shared_ptr<arrow::dataset::ScanTask> scan_task = vector.at(i);
    jlong id[] = {scan_task_holder_.Insert(scan_task)};
    env->SetLongArrayRegion(ret, i, 1, id);
  }
  return ret;
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeScanner
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeScanner
    (JNIEnv *, jobject, jlong scanner_id) {
  scanner_holder_.Erase(scanner_id);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeScanTask
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeScanTask
    (JNIEnv *, jobject, jlong id) {
  scan_task_holder_.Erase(id);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    scan
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_scan
    (JNIEnv* env, jobject, jlong scan_task_id) {
  std::shared_ptr<arrow::dataset::ScanTask> scan_task = scan_task_holder_.Lookup(scan_task_id);
  JNI_ASSIGN_OR_THROW(arrow::RecordBatchIterator record_batch_iterator, scan_task->Execute())
  return iterator_holder_
      .Insert(std::make_shared<arrow::RecordBatchIterator>(std::move(record_batch_iterator))); // move and propagate
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    nextRecordBatch
 * Signature: (J)Lorg/apache/arrow/dataset/jni/NativeRecordBatchHandle;
 */
JNIEXPORT jobject JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_nextRecordBatch
    (JNIEnv* env, jobject, jlong iterator_id) {
  std::shared_ptr<arrow::RecordBatchIterator> itr = iterator_holder_.Lookup(iterator_id);

  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::RecordBatch> record_batch, itr->Next())
  if (record_batch == nullptr) {
    return nullptr; // stream ended
  }
  std::shared_ptr<arrow::Schema> schema = record_batch->schema();
  jobjectArray field_array =
      env->NewObjectArray(schema->num_fields(), record_batch_handle_field_class, nullptr);

  std::vector<std::shared_ptr<arrow::Buffer>> buffers;
  for (int i = 0; i < schema->num_fields(); ++i) {
    auto column = record_batch->column(i);
    auto dataArray = column->data();
    jobject field = env->NewObject(record_batch_handle_field_class, record_batch_handle_field_constructor,
                                   column->length(), column->null_count());
    env->SetObjectArrayElement(field_array, i, field);

    for (auto& buffer : dataArray->buffers) {
      buffers.push_back(buffer);
    }
  }

  jobjectArray buffer_array =
      env->NewObjectArray(buffers.size(), record_batch_handle_buffer_class, nullptr);

  for (size_t j = 0; j < buffers.size(); ++j) {
    auto buffer = buffers[j];
    uint8_t* data = nullptr;
    int64_t size = 0;
    int64_t capacity = 0;
    if (buffer != nullptr) {
      data = (uint8_t*) buffer->data();
      size = buffer->size();
      capacity = buffer->capacity();
    }
    jobject buffer_handle = env->NewObject(record_batch_handle_buffer_class, record_batch_handle_buffer_constructor,
                                           buffer_holder_.Insert(buffer), data,
                                           size, capacity);
    env->SetObjectArrayElement(buffer_array, j, buffer_handle);
  }

  jobject ret = env->NewObject(record_batch_handle_class, record_batch_handle_constructor,
                               record_batch->num_rows(), field_array, buffer_array);
  return ret;
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    closeIterator
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_closeIterator
    (JNIEnv *, jobject, jlong id) {
  iterator_holder_.Erase(id);
}

/*
 * Class:     org_apache_arrow_dataset_jni_JniWrapper
 * Method:    releaseBuffer
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_apache_arrow_dataset_jni_JniWrapper_releaseBuffer
    (JNIEnv *, jobject, jlong id) {
  buffer_holder_.Erase(id);
}


/*
 * Class:     org_apache_arrow_dataset_file_JniWrapper
 * Method:    makeSingleFileDatasetFactory
 * Signature: (Ljava/lang/String;II)J
 */
JNIEXPORT jlong JNICALL Java_org_apache_arrow_dataset_file_JniWrapper_makeSingleFileDatasetFactory
    (JNIEnv* env, jobject, jstring path, jint file_format_id, jint file_system_id) {
  std::shared_ptr<arrow::dataset::FileFormat> file_format = GetFileFormat(env, file_format_id);
  std::string out_path;
  std::shared_ptr<arrow::fs::FileSystem> fs
      = GetFileSystem(env, file_system_id, JStringToCString(env, path), &out_path);
  JNI_ASSIGN_OR_THROW(std::shared_ptr<arrow::dataset::DatasetFactory> d,
                      arrow::dataset::SingleFileDatasetFactory::Make(out_path, fs, file_format))
  return dataset_factory_holder_.Insert(d);
}