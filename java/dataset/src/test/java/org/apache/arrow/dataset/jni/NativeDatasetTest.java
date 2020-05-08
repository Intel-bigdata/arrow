/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.arrow.dataset.jni;

import java.io.File;
import java.util.Iterator;
import java.util.List;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.stream.Collectors;
import java.util.stream.StreamSupport;

import org.apache.arrow.dataset.DatasetTypes;
import org.apache.arrow.dataset.file.FileFormat;
import org.apache.arrow.dataset.file.FileSystem;
import org.apache.arrow.dataset.file.SingleFileDatasetFactory;
import org.apache.arrow.dataset.filter.Filter;
import org.apache.arrow.dataset.filter.FilterImpl;
import org.apache.arrow.dataset.fragment.DataFragment;
import org.apache.arrow.dataset.scanner.ScanOptions;
import org.apache.arrow.dataset.scanner.ScanTask;
import org.apache.arrow.dataset.scanner.Scanner;
import org.apache.arrow.dataset.source.Dataset;
import org.apache.arrow.dataset.source.DatasetFactory;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.types.pojo.Schema;
import org.junit.Assert;
import org.junit.Ignore;
import org.junit.Test;

public class NativeDatasetTest {

  private String sampleParquet() {
    return NativeDatasetTest.class.getResource(File.separator + "userdata1.parquet").getPath();
  }

  private void testDatasetFactoryEndToEnd(DatasetFactory factory) {
    Schema schema = factory.inspect();

    Assert.assertEquals("Schema<registration_dttm: Timestamp(NANOSECOND, null), id: Int(32, true), " +
        "first_name: Utf8, last_name: Utf8, email: Utf8, gender: Utf8, ip_address: Utf8, cc: Utf8, " +
        "country: Utf8, birthdate: Utf8, salary: FloatingPoint(DOUBLE), title: Utf8, comments: Utf8>",
        schema.toString());

    Dataset dataset = factory.finish();
    Assert.assertNotNull(dataset);

    List<? extends DataFragment> fragments = collect(
        dataset.getFragments(new ScanOptions(new String[0], Filter.EMPTY, 100)));
    Assert.assertEquals(1, fragments.size());

    DataFragment fragment = fragments.get(0);
    List<? extends ScanTask> scanTasks = collect(fragment.scan());
    Assert.assertEquals(1, scanTasks.size());

    ScanTask scanTask = scanTasks.get(0);
    List<? extends VectorSchemaRoot> data = collect(scanTask.scan());
    Assert.assertNotNull(data);
    // 1000 rows total in file userdata1.parquet
    Assert.assertEquals(10, data.size());
    VectorSchemaRoot vsr = data.get(0);
    Assert.assertEquals(100, vsr.getRowCount());


    // FIXME when using list field:
    // FIXME it seems c++ parquet reader doesn't create buffers for list field it self,
    // FIXME as a result Java side buffer pointer gets out of bound.
  }

  @Test
  public void testLocalFs() {
    String path = sampleParquet();
    DatasetFactory discovery = new SingleFileDatasetFactory(
        new RootAllocator(Long.MAX_VALUE), FileFormat.PARQUET, FileSystem.LOCAL,
        path);
    testDatasetFactoryEndToEnd(discovery);
  }

  @Test
  public void testHdfsWithFileProtocol() {
    String path = "file:" + sampleParquet();
    DatasetFactory discovery = new SingleFileDatasetFactory(
        new RootAllocator(Long.MAX_VALUE), FileFormat.PARQUET, FileSystem.HDFS,
        path);
    testDatasetFactoryEndToEnd(discovery);
  }

  @Test
  public void testHdfsWithHdfsProtocol() {
    // If using libhdfs rather than libhdfs3:
    // Set JAVA_HOME and HADOOP_HOME first. See hdfs_internal.cc:128
    // And libhdfs requires having hadoop java libraries set within CLASSPATH. See 1. https://hadoop.apache.org/docs/r2.7.7/hadoop-project-dist/hadoop-hdfs/LibHdfs.html, 2. https://arrow.apache.org/docs/python/filesystems.html#hadoop-file-system-hdfs

    // If using libhdfs3, make sure ARROW_LIBHDFS3_DIR is set.
    // Install libhdfs3: https://medium.com/@arush.xtremelife/connecting-hadoop-hdfs-with-python-267234bb68a2
    String path = "hdfs://localhost:9000/userdata1.parquet?use_hdfs3=1";
    DatasetFactory discovery = new SingleFileDatasetFactory(
        new RootAllocator(Long.MAX_VALUE), FileFormat.PARQUET, FileSystem.HDFS,
        path);
    testDatasetFactoryEndToEnd(discovery);
  }

  @Test
  public void testScanner() {
    String path = sampleParquet();
    RootAllocator allocator = new RootAllocator(Long.MAX_VALUE);
    NativeDatasetFactory factory = new SingleFileDatasetFactory(
        allocator, FileFormat.PARQUET, FileSystem.LOCAL,
        path);
    Schema schema = factory.inspect();
    NativeDataset dataset = factory.finish(schema);
    ScanOptions scanOptions = new ScanOptions(new String[]{"id", "title"}, Filter.EMPTY, 100);
    Scanner scanner = dataset.newScan(scanOptions);
    List<? extends ScanTask> scanTasks = collect(scanner.scan());
    Assert.assertEquals(1, scanTasks.size());

    ScanTask scanTask = scanTasks.get(0);
    ScanTask.Itr itr = scanTask.scan();
    int vsrCount = 0;
    VectorSchemaRoot vsr = null;
    while (itr.hasNext()) {
      // FIXME VectorSchemaRoot is not actually something ITERABLE.// Using a reader convention instead.
      vsrCount++;
      vsr = itr.next();
      Assert.assertEquals(100, vsr.getRowCount());

      // check if projector is applied
      Assert.assertEquals("Schema<id: Int(32, true), title: Utf8>",
          vsr.getSchema().toString());
    }
    Assert.assertEquals(10, vsrCount);

    if (vsr != null) {
      vsr.close();
    }
    allocator.close();
  }

  @Test
  public void testScannerWithFilter() {
    String path = sampleParquet();
    RootAllocator allocator = new RootAllocator(Long.MAX_VALUE);
    NativeDatasetFactory factory = new SingleFileDatasetFactory(
        allocator, FileFormat.PARQUET, FileSystem.LOCAL,
        path);
    Schema schema = factory.inspect();
    NativeDataset dataset = factory.finish(schema);
    // condition id = 500
    DatasetTypes.Condition condition = DatasetTypes.Condition.newBuilder()
        .setRoot(DatasetTypes.TreeNode.newBuilder()
            .setCpNode(DatasetTypes.ComparisonNode.newBuilder()
                .setOpName("equal") // todo make op names enumerable
                .setLeftArg(
                    DatasetTypes.TreeNode.newBuilder().setFieldNode(
                        DatasetTypes.FieldNode.newBuilder().setName("id").build()).build())
                .setRightArg(
                    DatasetTypes.TreeNode.newBuilder().setIntNode(
                        DatasetTypes.IntNode.newBuilder().setValue(500).build()).build())
                .build())
            .build())
        .build();
    Filter filter = new FilterImpl(condition);
    ScanOptions scanOptions = new ScanOptions(new String[]{"id", "title"}, filter, 100);
    Scanner scanner = dataset.newScan(scanOptions);
    List<? extends ScanTask> scanTasks = collect(scanner.scan());
    Assert.assertEquals(1, scanTasks.size());

    ScanTask scanTask = scanTasks.get(0);
    ScanTask.Itr itr = scanTask.scan();
    VectorSchemaRoot vsr = null;
    int rowCount = 0;
    while (itr.hasNext()) {
      // FIXME VectorSchemaRoot is not actually something ITERABLE. Using a reader convention instead.
      vsr = itr.next();
      // only the line with id = 500 selected
      rowCount += vsr.getRowCount();

      // check if projector is applied
      Assert.assertEquals("Schema<id: Int(32, true), title: Utf8>",
          vsr.getSchema().toString());
    }
    Assert.assertEquals(1, rowCount);

    if (vsr != null) {
      vsr.close();
    }
    allocator.close();
  }

  // TODO fix for empty projector. Currently empty projector is treated as projection on all available columns.
  @Ignore
  public void testScannerWithEmptyProjector() {
    String path = sampleParquet();
    RootAllocator allocator = new RootAllocator(Long.MAX_VALUE);
    NativeDatasetFactory factory = new SingleFileDatasetFactory(
        allocator, FileFormat.PARQUET, FileSystem.LOCAL,
        path);
    Schema schema = factory.inspect();
    NativeDataset dataset = factory.finish(schema);
    ScanOptions scanOptions = new ScanOptions(new String[]{}, Filter.EMPTY, 100);
    Scanner scanner = dataset.newScan(scanOptions);
    List<? extends ScanTask> scanTasks = collect(scanner.scan());
    Assert.assertEquals(1, scanTasks.size());

    ScanTask scanTask = scanTasks.get(0);
    ScanTask.Itr itr = scanTask.scan();
    VectorSchemaRoot vsr = null;
    int rowCount = 0;
    while (itr.hasNext()) {
      // FIXME VectorSchemaRoot is not actually something ITERABLE. Using a reader convention instead.
      vsr = itr.next();
      rowCount += vsr.getRowCount();

      // check if projector is applied
      Assert.assertEquals("Schema<>",
          vsr.getSchema().toString());
    }
    Assert.assertEquals(1, rowCount);

    if (vsr != null) {
      vsr.close();
    }
    allocator.close();
  }

  @Ignore
  public void testFilter() {
    // todo
  }

  @Ignore
  public void testProjector() {
    // todo
  }

  private <T> List<T> collect(Iterable<T> iterable) {
    return StreamSupport.stream(iterable.spliterator(), false).collect(Collectors.toList());
  }

  private <T> List<T> collect(Iterator<T> iterator) {
    return StreamSupport.stream(Spliterators.spliteratorUnknownSize(iterator, Spliterator.ORDERED), false)
        .collect(Collectors.toList());
  }
}
