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

package org.apache.arrow.memory;

/**
 * Utility class managing ownership's transferring between Native Arrow buffers and Java Arrow buffers.
 */
public class Ownerships {
  private static final Ownerships INSTANCE = new Ownerships();

  private Ownerships() {
  }

  public static Ownerships get() {
    return INSTANCE;
  }

  /**
   * Returned ledger's ref count is initialized or increased by 1.
   */
  public BufferLedger takeOwnership(BaseAllocator allocator, AllocationManager allocationManager) {
    final BufferLedger ledger = allocationManager.associate(allocator);
    boolean succeed = allocator.forceAllocate(allocationManager.getSize());
    if (!succeed) {
      throw new OutOfMemoryException("Target allocator is full");
    }
    allocationManager.setOwningLedger(ledger);
    return ledger;
  }
}
