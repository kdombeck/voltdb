/* This file is part of VoltDB.
 * Copyright (C) 2008-2014 VoltDB Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

package txnIdSelfCheck;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;
import java.util.Random;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

import org.voltcore.logging.VoltLogger;
import org.voltdb.ClientResponseImpl;
import org.voltdb.TheHashinator;
import org.voltdb.VoltTable;
import org.voltdb.VoltType;
import org.voltdb.client.Client;
import org.voltdb.client.ClientResponse;
import org.voltdb.client.NoConnectionsException;
import org.voltdb.client.ProcCallException;
import org.voltdb.client.ProcedureCallback;

/**
 * The LoadTableLoader gets passed in with tableName for which the thread does loading of data using sysproc. For MP
 * LoadMultiPartitionTable is used and for SP LaodSinglePartitionTable is used. The thread also launches a CopyAndDelete
 * thread which copies and deletes data from Load*Table tables 1/3rd of the time. This is to sprinkle other
 * transactions. The CopyAndDelete also servers the purpose of verification that the Load*Table data got indeed loaded.
 *
 * Any procedure failure will fail the test and exit.
 */
public class LoadTableLoader extends Thread {

    static VoltLogger log = new VoltLogger("HOST");

    final Client client;
    final long targetCount;
    final String m_tableName;
    final int batchSize;
    final Random r;
    final AtomicBoolean m_shouldContinue = new AtomicBoolean(true);
    final boolean m_isMP;
    final Semaphore m_permits;

    //Column information
    private VoltTable.ColumnInfo m_colInfo[];
    //Zero based index of the partitioned column
    private int m_partitionedColumnIndex = -1;
    //proc name
    final String m_procName;
    //proc name for load table row delete
    final String m_onlydelprocName;
    //Table that keeps building.
    final VoltTable m_table;
    final Random m_random;
    final AtomicLong currentRowCount = new AtomicLong(0);
    final BlockingQueue<Long> cpDelQueue = new LinkedBlockingQueue<Long>();

    LoadTableLoader(Client client, String tableName, int targetCount, int batchSize, Semaphore permits, boolean isMp, int pcolIdx)
            throws IOException, ProcCallException {
        setName("LoadTableLoader-" + tableName);
        setDaemon(true);

        this.client = client;
        this.m_tableName = tableName;
        this.targetCount = targetCount;
        this.batchSize = batchSize;
        m_permits = permits;
        m_partitionedColumnIndex = pcolIdx;
        m_isMP = isMp;
        //Build column info so we can build VoltTable
        m_colInfo = new VoltTable.ColumnInfo[3];
        m_colInfo[0] = new VoltTable.ColumnInfo("cid", VoltType.BIGINT);
        m_colInfo[1] = new VoltTable.ColumnInfo("txnid", VoltType.BIGINT);
        m_colInfo[2] = new VoltTable.ColumnInfo("rowid", VoltType.BIGINT);
        m_procName = (m_isMP ? "@LoadMultipartitionTable" : "@LoadSinglepartitionTable");
        m_onlydelprocName = (m_isMP ? "DeleteOnlyLoadTableMP" : "DeleteOnlyLoadTableSP");
        m_table = new VoltTable(m_colInfo);
        long curtmms = Calendar.getInstance().getTimeInMillis();
        m_random = new Random(curtmms);
        r = new Random(curtmms + 1);

        System.out.println("LoadTableLoader Table " + m_tableName + " Is : " + (m_isMP ? "MP" : "SP") + " Target Count: " + targetCount);
        // make this run more than other threads
        setPriority(getPriority() + 1);
    }

    long getRowCount() throws NoConnectionsException, IOException, ProcCallException {
        VoltTable t = client.callProcedure("@AdHoc", "select count(*) from " + m_tableName + ";").getResults()[0];
        return t.asScalarLong();
    }

    void shutdown() {
        m_shouldContinue.set(false);
    }

    class InsertCallback implements ProcedureCallback {

        CountDownLatch latch;

        InsertCallback(CountDownLatch latch) {
            this.latch = latch;
        }

        @Override
        public void clientCallback(ClientResponse clientResponse) throws Exception {
            latch.countDown();
            byte status = clientResponse.getStatus();
            if (status == ClientResponse.GRACEFUL_FAILURE) {
                // log what happened
                log.error("LoadTableLoader gracefully failed to insert into table " + m_tableName + " and this shoudn't happen. Exiting.");
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the world
                System.exit(-1);
            }
            if (status != ClientResponse.SUCCESS) {
                // log what happened
                log.error("LoadTableLoader ungracefully failed to insert into table " + m_tableName);
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the loader
                m_shouldContinue.set(false);
            }
        }
    }

    class InsertCopyCallback implements ProcedureCallback {

        CountDownLatch latch;

        InsertCopyCallback(CountDownLatch latch) {
            this.latch = latch;
        }

        @Override
        public void clientCallback(ClientResponse clientResponse) throws Exception {
            currentRowCount.incrementAndGet();
            latch.countDown();
            byte status = clientResponse.getStatus();
            if (status == ClientResponse.GRACEFUL_FAILURE) {
                // log what happened
                log.error("LoadTableLoader gracefully failed to copy from table " + m_tableName + " and this shoudn't happen. Exiting.");
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the world
                System.exit(-1);
            }
            if (status != ClientResponse.SUCCESS) {
                // log what happened
                log.error("LoadTableLoader ungracefully failed to copy from table " + m_tableName);
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the loader
                m_shouldContinue.set(false);
            }
        }
    }

    class DeleteCallback implements ProcedureCallback {

        final CountDownLatch latch;
        final int expected_delete;

        DeleteCallback(CountDownLatch latch, int expected_delete) {
            this.latch = latch;
            this.expected_delete = expected_delete;
        }

        @Override
        public void clientCallback(ClientResponse clientResponse) throws Exception {
            latch.countDown();
            byte status = clientResponse.getStatus();
            if (status == ClientResponse.GRACEFUL_FAILURE) {
                // log what happened
                log.error("LoadTableLoader gracefully failed to delete from table " + m_tableName + " and this shoudn't happen. Exiting.");
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the world
                System.exit(-1);
            }
            if (status != ClientResponse.SUCCESS) {
                // log what happened
                log.error("LoadTableLoader ungracefully failed to copy from table " + m_tableName);
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the loader
                m_shouldContinue.set(false);
            }
            long cnt = clientResponse.getResults()[0].asScalarLong();
            if (cnt != expected_delete) {
                log.error("LoadTableLoader ungracefully failed to delete: " + m_tableName + " count=" + cnt);
                log.error(((ClientResponseImpl) clientResponse).toJSONString());
                // stop the loader
                m_shouldContinue.set(false);
            }
        }
    }

    //Local task thread doing copy and delete under the loadtable tables.
    class CopyAndDeleteDataTask extends Thread {

        final AtomicBoolean m_shouldContinue = new AtomicBoolean(true);
        int m_copyDeleteDoneCount = 0;
        //proc name for copy
        final String m_cpprocName;
        //proc name for 2 delete
        final String m_delprocName;

        void shutdown() {
            m_shouldContinue.set(false);
        }

        public CopyAndDeleteDataTask() {
            setName("CopyAndDeleteDataTask-" + m_tableName);
            setDaemon(true);
            m_cpprocName = (m_isMP ? "CopyLoadPartitionedMP" : "CopyLoadPartitionedSP");
            m_delprocName = (m_isMP ? "DeleteLoadPartitionedMP" : "DeleteLoadPartitionedSP");
        }

        @Override
        public void run() {
            System.out.println("Starting Copy Delete Task for table: " + m_tableName);
            try {
                List<Long> workList = new ArrayList<Long>();
                while (m_shouldContinue.get()) {
                    cpDelQueue.drainTo(workList, 10);
                    if (workList.size() <= 0) {
                        Thread.sleep(2000);
                    }
                    CountDownLatch clatch = new CountDownLatch(workList.size());
                    for (Long lcid : workList) {
                        client.callProcedure(new InsertCopyCallback(clatch), m_cpprocName, lcid);
                    }
                    clatch.await();
                    CountDownLatch dlatch = new CountDownLatch(workList.size());
                    for (Long lcid : workList) {
                        client.callProcedure(new DeleteCallback(dlatch, 2), m_delprocName, lcid);
                    }
                    dlatch.await();
                    m_copyDeleteDoneCount += workList.size();
                    workList.clear();
                }
                System.out.println("CopyAndDeleteTask row count: " + m_copyDeleteDoneCount);
            } catch (Exception e) {
                // on exception, log and end the thread, but don't kill the process
                log.error("CopyAndDeleteDataTask failed a procedure call for table " + m_tableName
                        + " and the thread will now stop.", e);
            }
        }
    }

    @Override
    public void run() {

        CopyAndDeleteDataTask cdtask = new CopyAndDeleteDataTask();
        cdtask.start();
        try {
            ArrayList<Long> onlyDeleteList = new ArrayList<Long>();
            while (m_shouldContinue.get()) {
                //1 in 3 gets copied and then deleted after leaving some data
                byte shouldCopy = (byte) (m_random.nextInt(3) == 0 ? 1 : 0);
                CountDownLatch latch = new CountDownLatch(batchSize);
                // try to insert batchSize random rows
                for (int i = 0; i < batchSize; i++) {
                    m_table.clearRowData();
                    m_permits.acquire();
                    long p = Math.abs(r.nextLong());
                    m_table.addRow(p, p, Calendar.getInstance().getTimeInMillis());
                    boolean success = false;
                    if (!m_isMP) {
                        Object rpartitionParam
                                = TheHashinator.valueToBytes(m_table.fetchRow(0).get(
                                                m_partitionedColumnIndex, VoltType.BIGINT));
                        success = client.callProcedure(new InsertCallback(latch), m_procName, rpartitionParam, m_tableName, m_table);
                    } else {
                        success = client.callProcedure(new InsertCallback(latch), m_procName, m_tableName, m_table);
                    }
                    if (success) {
                        if (shouldCopy != 0) {
                            cpDelQueue.add(p);
                        } else {
                            onlyDeleteList.add(p);
                        }
                    }
                }
                latch.await();
                long nextRowCount = getRowCount();
                // if no progress, throttle a bit
                if (nextRowCount == currentRowCount.get()) {
                    Thread.sleep(1000);
                }
                if (onlyDeleteList.size() > 100) {
                    CountDownLatch odlatch = new CountDownLatch(onlyDeleteList.size());
                    for (Long lcid : onlyDeleteList) {
                        client.callProcedure(new DeleteCallback(odlatch, 1), m_onlydelprocName, lcid);
                    }
                    odlatch.await();
                    onlyDeleteList.clear();
                }
            }
            //Any accumulated in p/mp tables are left behind.
        }
        catch (Exception e) {
            // on exception, log and end the thread, but don't kill the process
            log.error("LoadTableLoader failed a procedure call for table " + m_tableName
                    + " and the thread will now stop.", e);
        } finally {
            cdtask.shutdown();
            try {
                cdtask.join();
            } catch (InterruptedException ex) {
                log.error("CopyDelete Task was stopped.", ex);
            }
        }
    }

}
