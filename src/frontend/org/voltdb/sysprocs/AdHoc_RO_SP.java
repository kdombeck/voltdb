/* This file is part of VoltDB.
 * Copyright (C) 2008-2012 VoltDB Inc.
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

package org.voltdb.sysprocs;

import org.voltdb.ProcInfo;
import org.voltdb.SystemProcedureExecutionContext;
import org.voltdb.VoltTable;

@ProcInfo(
    singlePartition = true,
    partitionInfo = "DUMMY: 4"
)
/**
 * Execute a user-provided read-only single-partition SQL statement.
 * This code coordinates the execution of the plan fragments generated by the
 * embedded planner process.
 *
 * AdHocBase implements the core logic.
 * This subclass is needed for @ProcInfo.
 */
public class AdHoc_RO_SP extends AdHocBase {

    /**
     * System procedure run hook.
     * Use the base class implementation.
     *
     * @param ctx  execution context
     * @param aggregatorFragments  aggregator plan fragments
     * @param collectorFragments  collector plan fragments
     * @param sqlStatements  source SQL statements
     * @param replicatedTableDMLFlags  flags set to 1 when replicated
     *
     * @return  results as VoltTable array
     */
    public VoltTable[] run(SystemProcedureExecutionContext ctx,
            String[] aggregatorFragments, String[] collectorFragments,
            String[] sqlStatements, int[] replicatedTableDMLFlags) {
        return runAdHoc(ctx, aggregatorFragments, collectorFragments, sqlStatements, replicatedTableDMLFlags);
    }

}