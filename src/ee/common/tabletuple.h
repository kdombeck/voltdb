/* This file is part of VoltDB.
 * Copyright (C) 2008-2014 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
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
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HSTORETABLETUPLE_H
#define HSTORETABLETUPLE_H

#include "common/common.h"
#include "common/TupleSchema.h"
#include "common/Pool.hpp"
#include "common/ValuePeeker.hpp"
#include "common/FatalException.hpp"
#include "common/ExportSerializeIo.h"

#include <cassert>
#include <ostream>
#include <iostream>

class CopyOnWriteTest_TestTableTupleFlags;

namespace voltdb {

#define TUPLE_HEADER_SIZE 1

#define ACTIVE_MASK 1
#define DIRTY_MASK 2
#define PENDING_DELETE_MASK 4
#define PENDING_DELETE_ON_UNDO_RELEASE_MASK 8

class TableColumn;
class TupleIterator;
class ElasticScanner;
class StandAloneTupleStorage;

class TableTuple {
    // friend access is intended to allow write access to the tuple flags -- try not to abuse it...
    friend class Table;
    friend class TempTable;
    friend class PersistentTable;
    friend class ElasticScanner;
    friend class PoolBackedTupleStorage;
    friend class CopyOnWriteIterator;
    friend class CopyOnWriteContext;
    friend class ::CopyOnWriteTest_TestTableTupleFlags;
    friend class StandAloneTupleStorage; // ... OK, this friend can also update m_schema.

public:
    /** Initialize a tuple unassociated with a table (bad idea... dangerous) */
    explicit TableTuple();

    /** Setup the tuple given a table */
    TableTuple(const TableTuple &rhs);

    /** Setup the tuple given a schema */
    TableTuple(const TupleSchema *schema);

    /** Setup the tuple given the specified data location and schema **/
    TableTuple(char *data, const voltdb::TupleSchema *schema);

    /** Assignment operator */
    TableTuple& operator=(const TableTuple &rhs);

    /**
     * Set the tuple to point toward a given address in a table's
     * backing store
     */
    inline void move(void *address) {
        assert(m_schema);
        m_data = reinterpret_cast<char*> (address);
    }

    inline void moveNoHeader(void *address) {
        assert(m_schema);
        // isActive() and all the other methods expect a header
        m_data = reinterpret_cast<char*> (address) - TUPLE_HEADER_SIZE;
    }

    // Used to wrap read only tuples in indexing code. TODO Remove
    // constedeness from indexing code so this cast isn't necessary.
    inline void moveToReadOnlyTuple(const void *address) {
        assert(m_schema);
        assert(address);
        //Necessary to move the pointer back TUPLE_HEADER_SIZE
        // artificially because Tuples used as keys for indexes do not
        // have the header.
        m_data = reinterpret_cast<char*>(const_cast<void*>(address)) - TUPLE_HEADER_SIZE;
    }

    /** Get the address of this tuple in the table's backing store */
    inline char* address() const {
        return m_data;
    }

    /** Return the number of columns in this tuple */
    inline int sizeInValues() const {
        return m_schema->columnCount();
    }

    /**
        Determine the maximum number of bytes when serialized for Export.
        Excludes the bytes required by the row header (which includes
        the null bit indicators) and ignores the width of metadata cols.
    */
    size_t maxExportSerializationSize() const {
        size_t bytes = 0;
        int cols = sizeInValues();
        for (int i = 0; i < cols; ++i) {
            switch (getType(i)) {
              case VALUE_TYPE_TINYINT:
              case VALUE_TYPE_SMALLINT:
              case VALUE_TYPE_INTEGER:
              case VALUE_TYPE_BIGINT:
              case VALUE_TYPE_TIMESTAMP:
              case VALUE_TYPE_DOUBLE:
                bytes += sizeof (int64_t);
                break;

              case VALUE_TYPE_DECIMAL:
                // decimals serialized in ascii as
                // 32 bits of length + max prec digits + radix pt + sign
                bytes += sizeof (int32_t) + NValue::kMaxDecPrec + 1 + 1;
                break;

              case VALUE_TYPE_VARCHAR:
              case VALUE_TYPE_VARBINARY:
                  // 32 bit length preceding value and
                  // actual character data without null string terminator.
                  if (!getNValue(i).isNull())
                  {
                      bytes += (sizeof (int32_t) +
                                ValuePeeker::peekObjectLength(getNValue(i)));
                  }
                break;
              default:
                // let caller handle this error
                throwDynamicSQLException(
                        "Unknown ValueType %s found during Export serialization.",
                        valueToString(getType(i)).c_str() );
                return (size_t)0;
            }
        }
        return bytes;
    }

    // Return the amount of memory allocated for non-inlined objects
    size_t getNonInlinedMemorySize() const
    {
        size_t bytes = 0;
        int cols = sizeInValues();
        // fast-path for no inlined cols
        if (m_schema->getUninlinedObjectColumnCount() != 0)
        {
            for (int i = 0; i < cols; ++i)
            {
                // peekObjectLength is unhappy with non-varchar
                if (((getType(i) == VALUE_TYPE_VARCHAR) || (getType(i) == VALUE_TYPE_VARBINARY)) &&
                    !m_schema->columnIsInlined(i))
                {
                    if (!getNValue(i).isNull())
                    {
                        bytes +=
                            StringRef::
                            computeStringMemoryUsed((ValuePeeker::
                                                     peekObjectLength(getNValue(i))));
                    }
                }
            }
        }
        return bytes;
    }

    void setNValue(const int idx, voltdb::NValue value);
    /*
     * Copies range of NValues from one tuple to another.
     */
    void setNValues(int beginIdx, TableTuple lhs, int begin, int end);

    /*
     * Version of setNValue that will allocate space to copy
     * strings that can't be inlined rather then copying the
     * pointer. Used when setting an NValue that will go into
     * permanent storage in a persistent table.  It is also possible
     * to provide NULL for stringPool in which case the strings will
     * be allocated on the heap.
     */
    void setNValueAllocateForObjectCopies(const int idx, voltdb::NValue value,
                                             Pool *dataPool);

    /** How long is a tuple? */
    inline int tupleLength() const {
        return m_schema->tupleLength() + TUPLE_HEADER_SIZE;
    }

    /** Is the tuple deleted or active? */
    inline bool isActive() const {
        return (*(reinterpret_cast<const char*> (m_data)) & ACTIVE_MASK) ? true : false;
    }

    /** Is the tuple deleted or active? */
    inline bool isDirty() const {
        return (*(reinterpret_cast<const char*> (m_data)) & DIRTY_MASK) ? true : false;
    }

    inline bool isPendingDelete() const {
        return (*(reinterpret_cast<const char*> (m_data)) & PENDING_DELETE_MASK) ? true : false;
    }

    inline bool isPendingDeleteOnUndoRelease() const {
        return (*(reinterpret_cast<const char*> (m_data)) & PENDING_DELETE_ON_UNDO_RELEASE_MASK) ? true : false;
    }

    /** Is the column value null? */
    inline bool isNull(const int idx) const {
        return getNValue(idx).isNull();
    }

    inline bool isNullTuple() const {
        return m_data == NULL;
    }

    /** Get the type of a particular column in the tuple */
    inline ValueType getType(int idx) const {
        return m_schema->columnType(idx);
    }

    /** Get the value of a specified column (const) */
    //not performant because it has to check the schema to see how to
    //return the SlimValue.
    inline const NValue getNValue(const int idx) const {
        assert(m_schema);
        assert(m_data);
        assert(idx < m_schema->columnCount());

        //assert(isActive());
        const voltdb::ValueType columnType = m_schema->columnType(idx);
        const char* dataPtr = getDataPtr(idx);
        const bool isInlined = m_schema->columnIsInlined(idx);
        return NValue::initFromTupleStorage(dataPtr, columnType, isInlined);
    }

    inline const voltdb::TupleSchema* getSchema() const {
        return m_schema;
    }

    /** Print out a human readable description of this tuple */
    std::string debug(const std::string& tableName) const;
    std::string debugNoHeader() const;

    /** Copy values from one tuple into another (uses memcpy) */
    // verify assumptions for copy. do not use at runtime (expensive)
    bool compatibleForCopy(const TableTuple &source);
    void copyForPersistentInsert(const TableTuple &source, Pool *pool = NULL);
    // The vector "output" arguments detail the non-inline object memory management
    // required of the upcoming release or undo.
    void copyForPersistentUpdate(const TableTuple &source,
                                 std::vector<char*> &oldObjects, std::vector<char*> &newObjects);
    void copy(const TableTuple &source);

    /** this does set NULL in addition to clear string count.*/
    void setAllNulls();

    bool equals(const TableTuple &other) const;
    bool equalsNoSchemaCheck(const TableTuple &other) const;

    int compare(const TableTuple &other) const;

    void deserializeFrom(voltdb::SerializeInput &tupleIn, Pool *stringPool);
    void serializeTo(voltdb::SerializeOutput &output);
    void serializeToExport(voltdb::ExportSerializeOutput &io,
                          int colOffset, uint8_t *nullArray);

    void freeObjectColumns();
    size_t hashCode(size_t seed) const;
    size_t hashCode() const;
private:
    inline void setActiveTrue() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) |= static_cast<char>(ACTIVE_MASK);
    }
    inline void setActiveFalse() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) &= static_cast<char>(~ACTIVE_MASK);
    }

    inline void setPendingDeleteOnUndoReleaseTrue() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) |= static_cast<char>(PENDING_DELETE_ON_UNDO_RELEASE_MASK);
    }
    inline void setPendingDeleteOnUndoReleaseFalse() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) &= static_cast<char>(~PENDING_DELETE_ON_UNDO_RELEASE_MASK);
    }

    inline void setPendingDeleteTrue() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) |= static_cast<char>(PENDING_DELETE_MASK);
    }
    inline void setPendingDeleteFalse() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) &= static_cast<char>(~PENDING_DELETE_MASK);
    }

    inline void setDirtyTrue() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) |= static_cast<char>(DIRTY_MASK);
    }
    inline void setDirtyFalse() {
        // treat the first "value" as a boolean flag
        *(reinterpret_cast<char*> (m_data)) &= static_cast<char>(~DIRTY_MASK);
    }

    /** The types of the columns in the tuple */
    const TupleSchema *m_schema;

    /**
     * The column data, padded at the front by 8 bytes
     * representing whether the tuple is active or deleted
     */
    char *m_data;

    inline char* getDataPtr(const int idx) {
        assert(m_schema);
        assert(m_data);
        return &m_data[m_schema->columnOffset(idx) + TUPLE_HEADER_SIZE];
    }

    inline const char* getDataPtr(const int idx) const {
        assert(m_schema);
        assert(m_data);
        return &m_data[m_schema->columnOffset(idx) + TUPLE_HEADER_SIZE];
    }
};

/**
 * Convenience class for Tuples that get their (inline) storage from a pool.
 * The pool is specified on initial allocation and retained for later reallocations.
 * The tuples can be used like normal tuples except for allocation/reallocation.
 * The caller takes responsibility for consistently using the specialized methods below for that.
 */
class PoolBackedTupleStorage {
public:
    PoolBackedTupleStorage(const TupleSchema* schema, Pool* pool) : m_tuple(schema), m_pool(pool) { }

    void allocateActiveTuple()
    {
        char* storage = reinterpret_cast<char*>(m_pool->allocateZeroes(m_tuple.getSchema()->tupleLength() + TUPLE_HEADER_SIZE));
        m_tuple.move(storage);
        m_tuple.setActiveTrue();
    }

    /** Operator conversion to get an access to the underline tuple.
     * To prevent clients from repointing the tuple to some other backing
     * storage via move()or address() calls the tuple is returned by value
     */
    operator TableTuple& () {
        return m_tuple;
    }

private:
    TableTuple m_tuple;
    Pool* m_pool;
};

// A small class to hold together a standalone tuple (not backed by any table)
// and the associated tuple storage memory to keep the actual data.
class StandAloneTupleStorage {
    public:
        /** Creates an uninitialized tuple */
        StandAloneTupleStorage() :
            m_tupleStorage(),m_tuple() {
        }

        /** Allocates enough memory for a given schema
         * and initialies tuple to point to this memory
         */
        explicit StandAloneTupleStorage(const TupleSchema* schema) :
            m_tupleStorage(), m_tuple() {
            init(schema);
        }

        /** Allocates enough memory for a given schema
         * and initialies tuple to point to this memory
         */
        void init(const TupleSchema* schema) {
            assert(schema != NULL);
            m_tupleStorage.reset(new char[schema->tupleLength() + TUPLE_HEADER_SIZE]);
            m_tuple.m_schema = schema;
            m_tuple.move(m_tupleStorage.get());
            m_tuple.setAllNulls();
            m_tuple.setActiveTrue();
        }

        /** Operator conversion to get an access to the underline tuple.
         * To prevent clients from repointing the tuple to some other backing
         * storage via move()or address() calls the tuple is returned by value
         */
        operator TableTuple () {
            return m_tuple;
        }

        operator TableTuple () const {
            return m_tuple;
        }

    private:

        boost::scoped_array<char> m_tupleStorage;
        TableTuple m_tuple;

};

inline TableTuple::TableTuple() :
    m_schema(NULL), m_data(NULL) {
}

inline TableTuple::TableTuple(const TableTuple &rhs) :
    m_schema(rhs.m_schema), m_data(rhs.m_data) {
}

inline TableTuple::TableTuple(const TupleSchema *schema) :
    m_schema(schema), m_data(NULL) {
    assert (m_schema);
}

/** Setup the tuple given the specified data location and schema **/
inline TableTuple::TableTuple(char *data, const voltdb::TupleSchema *schema) {
    assert(data);
    assert(schema);
    m_data = data;
    m_schema = schema;
}

inline TableTuple& TableTuple::operator=(const TableTuple &rhs) {
    m_schema = rhs.m_schema;
    m_data = rhs.m_data;
    return *this;
}

/** Copy scalars by value and non-scalars (non-inlined strings, decimals) by
    reference from a slim value in to this tuple. */
inline void TableTuple::setNValue(const int idx, voltdb::NValue value) {
    assert(m_schema);
    assert(m_data);
    const ValueType type = m_schema->columnType(idx);
    value = value.castAs(type);
    const bool isInlined = m_schema->columnIsInlined(idx);
    char *dataPtr = getDataPtr(idx);
    const int32_t columnLength = m_schema->columnLength(idx);
    value.serializeToTupleStorage(dataPtr, isInlined, columnLength);
}

/** Multi column version. */
inline void TableTuple::setNValues(int beginIdx, TableTuple lhs, int begin, int end) {
    assert(m_schema);
    assert(lhs.getSchema());
    assert(beginIdx + end - begin <= sizeInValues());
    while (begin != end) {
        assert(m_schema->columnType(beginIdx) == lhs.getSchema()->columnType(begin));
        setNValue(beginIdx++, lhs.getNValue(begin++));
    }
}

/* Copy strictly by value from slimvalue into this tuple */
inline void TableTuple::setNValueAllocateForObjectCopies(const int idx,
                                                            voltdb::NValue value,
                                                            Pool *dataPool)
{
    assert(m_schema);
    assert(m_data);
    //assert(isActive())
    const ValueType type = m_schema->columnType(idx);
    value = value.castAs(type);
    const bool isInlined = m_schema->columnIsInlined(idx);
    char *dataPtr = getDataPtr(idx);
    const int32_t columnLength = m_schema->columnLength(idx);
    value.serializeToTupleStorageAllocateForObjects(dataPtr, isInlined,
                                                    columnLength, dataPool);
}

/*
 * With a persistent insert the copy should do an allocation for all uninlinable strings
 */
inline void TableTuple::copyForPersistentInsert(const voltdb::TableTuple &source, Pool *pool) {
    assert(m_schema);
    assert(source.m_schema);
    assert(source.m_data);
    assert(m_data);

    const bool allowInlinedObjects = m_schema->allowInlinedObjects();
    const TupleSchema *sourceSchema = source.m_schema;
    const bool oAllowInlinedObjects = sourceSchema->allowInlinedObjects();
    const uint16_t uninlineableObjectColumnCount = m_schema->getUninlinedObjectColumnCount();

#ifndef NDEBUG
    if(!compatibleForCopy(source)) {
        std::ostringstream message;
        message << "src  tuple: " << source.debug("") << std::endl;
        message << "src schema: " << source.m_schema->debug() << std::endl;
        message << "dest schema: " << m_schema->debug() << std::endl;
        throwFatalException( "%s", message.str().c_str());
    }
#endif

    if (allowInlinedObjects == oAllowInlinedObjects) {
        /*
         * The source and target tuple have the same policy WRT to
         * inlining strings. A memcpy can be used to speed the process
         * up for all columns that are not uninlineable strings.
         */
        if (uninlineableObjectColumnCount > 0) {
            // copy the data AND the isActive flag
            ::memcpy(m_data, source.m_data, m_schema->tupleLength() + TUPLE_HEADER_SIZE);
            /*
             * Copy each uninlined string column doing an allocation for string copies.
             */
            for (uint16_t ii = 0; ii < uninlineableObjectColumnCount; ii++) {
                const uint16_t uinlineableObjectColumnIndex =
                  m_schema->getUninlinedObjectColumnInfoIndex(ii);
                setNValueAllocateForObjectCopies(uinlineableObjectColumnIndex,
                                                    source.getNValue(uinlineableObjectColumnIndex),
                                                    pool);
            }
            m_data[0] = source.m_data[0];
        } else {
            // copy the data AND the isActive flag
            ::memcpy(m_data, source.m_data, m_schema->tupleLength() + TUPLE_HEADER_SIZE);
        }
    } else {
        // Can't copy the string ptr from the other tuple if the string
        // is inlined into the tuple
        assert(!(!allowInlinedObjects && oAllowInlinedObjects));
        const uint16_t columnCount = m_schema->columnCount();
        for (uint16_t ii = 0; ii < columnCount; ii++) {
            setNValueAllocateForObjectCopies(ii, source.getNValue(ii), pool);
        }

        m_data[0] = source.m_data[0];
    }
}

/*
 * With a persistent update the copy should only do an allocation for
 * a string if the source and destination pointers are different.
 */
inline void TableTuple::copyForPersistentUpdate(const TableTuple &source,
                                                std::vector<char*> &oldObjects, std::vector<char*> &newObjects)
{
    assert(m_schema);
    assert(m_schema == source.m_schema);
    const int columnCount = m_schema->columnCount();
    const uint16_t uninlineableObjectColumnCount = m_schema->getUninlinedObjectColumnCount();
    /*
     * The source and target tuple have the same policy WRT to
     * inlining strings because a TableTuple used for updating a
     * persistent table uses the same schema as the persistent table.
     */
    if (uninlineableObjectColumnCount > 0) {
        uint16_t uninlineableObjectColumnIndex = 0;
        uint16_t nextUninlineableObjectColumnInfoIndex = m_schema->getUninlinedObjectColumnInfoIndex(0);
        /*
         * Copy each column doing an allocation for string
         * copies. Compare the source and target pointer to see if it
         * is changed in this update. If it is changed then free the
         * old string and copy/allocate the new one from the source.
         */
        for (uint16_t ii = 0; ii < columnCount; ii++) {
            if (ii == nextUninlineableObjectColumnInfoIndex) {
                char *       *mPtr = reinterpret_cast<char**>(getDataPtr(ii));
                char * const *oPtr = reinterpret_cast<char* const*>(source.getDataPtr(ii));
                if (*mPtr != *oPtr) {
                    // Make a copy of the input string. Don't want to delete the old string
                    // because it's either from the temp pool or persistently referenced elsewhere.
                    oldObjects.push_back(*mPtr);
                    // TODO: Here, it's known that the column is an object type, and yet
                    // setNValueAllocateForObjectCopies is called to figure this all out again.
                    setNValueAllocateForObjectCopies(ii, source.getNValue(ii), NULL);
                    // Yes, uses the same old pointer as two statements ago to get a new value. Neat.
                    newObjects.push_back(*mPtr);
                }
                uninlineableObjectColumnIndex++;
                if (uninlineableObjectColumnIndex < uninlineableObjectColumnCount) {
                    nextUninlineableObjectColumnInfoIndex =
                      m_schema->getUninlinedObjectColumnInfoIndex(uninlineableObjectColumnIndex);
                } else {
                    // This is completely optional -- the value from here on has to be one that can't
                    // be reached by incrementing from the current value.
                    // Zero works, but then again so does the current value.
                    nextUninlineableObjectColumnInfoIndex = 0;
                }
            } else {
                // TODO: Here, it's known that the column value is some kind of scalar or inline, yet
                // setNValueAllocateForObjectCopies is called to figure this all out again.
                // This seriously complicated function is going to boil down to an incremental
                // memcpy of a few more bytes of the tuple.
                // Solution? It would likely be faster even for object-heavy tuples to work in three passes:
                // 1) collect up all the "changed object pointer" offsets.
                // 2) do the same wholesale tuple memcpy as in the no-objects "else" clause, below,
                // 3) replace the object pointer at each "changed object pointer offset"
                //    with a pointer to an object copy of its new referent.
                setNValueAllocateForObjectCopies(ii, source.getNValue(ii), NULL);
            }
        }
        // This obscure assignment is propagating the tuple flags rather than leaving it to the caller.
        // TODO: It would be easier for the caller to simply set the values it wants upon return.
        m_data[0] = source.m_data[0];
    } else {
        // copy the tuple flags and the data (all inline/scalars)
        ::memcpy(m_data, source.m_data, m_schema->tupleLength() + TUPLE_HEADER_SIZE);
    }
}

inline void TableTuple::copy(const TableTuple &source) {
    assert(m_schema);
    assert(source.m_schema);
    assert(source.m_data);
    assert(m_data);

    const uint16_t columnCount = m_schema->columnCount();
    const bool allowInlinedObjects = m_schema->allowInlinedObjects();
    const TupleSchema *sourceSchema = source.m_schema;
    const bool oAllowInlinedObjects = sourceSchema->allowInlinedObjects();

#ifndef NDEBUG
    if(!compatibleForCopy(source)) {
        std::ostringstream message;
        message << "src  tuple: " << source.debug("") << std::endl;
        message << "src schema: " << source.m_schema->debug() << std::endl;
        message << "dest schema: " << m_schema->debug() << std::endl;
        throwFatalException("%s", message.str().c_str());
    }
#endif

    if (allowInlinedObjects == oAllowInlinedObjects) {
        // copy the data AND the isActive flag
        ::memcpy(m_data, source.m_data, m_schema->tupleLength() + TUPLE_HEADER_SIZE);
    } else {
        // Can't copy the string ptr from the other tuple if the
        // string is inlined into the tuple
        assert(!(!allowInlinedObjects && oAllowInlinedObjects));
        for (uint16_t ii = 0; ii < columnCount; ii++) {
            setNValue(ii, source.getNValue(ii));
        }
        m_data[0] = source.m_data[0];
    }
}

inline void TableTuple::deserializeFrom(voltdb::SerializeInput &tupleIn, Pool *dataPool) {
    assert(m_schema);
    assert(m_data);

    tupleIn.readInt();
    for (int j = 0; j < m_schema->columnCount(); ++j) {
        const ValueType type = m_schema->columnType(j);
        /**
         * Hack hack. deserializeFrom is only called when we serialize
         * and deserialize tables. The serialization format for
         * Strings/Objects in a serialized table happens to have the
         * same in memory representation as the Strings/Objects in a
         * tabletuple. The goal here is to wrap the serialized
         * representation of the value in an NValue and then serialize
         * that into the tuple from the NValue. This makes it possible
         * to push more value specific functionality out of
         * TableTuple. The memory allocation will be performed when
         * serializing to tuple storage.
         */
        const bool isInlined = m_schema->columnIsInlined(j);
        char *dataPtr = getDataPtr(j);
        const int32_t columnLength = m_schema->columnLength(j);
        NValue::deserializeFrom(tupleIn, type, dataPtr, isInlined, columnLength, dataPool);
    }
}

inline void TableTuple::serializeTo(voltdb::SerializeOutput &output) {
    size_t start = output.reserveBytes(4);

    for (int j = 0; j < m_schema->columnCount(); ++j) {
        //int fieldStart = output.position();
        NValue value = getNValue(j);
        value.serializeTo(output);
    }

    // write the length of the tuple
    output.writeIntAt(start, static_cast<int32_t>(output.position() - start - sizeof(int32_t)));
}

inline
void
TableTuple::serializeToExport(ExportSerializeOutput &io,
                              int colOffset, uint8_t *nullArray)
{
    int columnCount = sizeInValues();
    for (int i = 0; i < columnCount; i++) {
        // NULL doesn't produce any bytes for the NValue
        // Handle it here to consolidate manipulation of
        // the nullarray.
        if (isNull(i)) {
            // turn on i'th bit of nullArray
            int byte = (colOffset + i) >> 3;
            int bit = (colOffset + i) % 8;
            int mask = 0x80 >> bit;
            nullArray[byte] = (uint8_t)(nullArray[byte] | mask);
            continue;
        }
        getNValue(i).serializeToExport(io);
    }
}

inline bool TableTuple::equals(const TableTuple &other) const {
    if (!m_schema->equals(other.m_schema)) {
        return false;
    }
    return equalsNoSchemaCheck(other);
}

inline bool TableTuple::equalsNoSchemaCheck(const TableTuple &other) const {
    for (int ii = 0; ii < m_schema->columnCount(); ii++) {
        const NValue lhs = getNValue(ii);
        const NValue rhs = other.getNValue(ii);
        if (lhs.op_notEquals(rhs).isTrue()) {
            return false;
        }
    }
    return true;
}

inline void TableTuple::setAllNulls() {
    assert(m_schema);
    assert(m_data);

    for (int ii = 0; ii < m_schema->columnCount(); ++ii) {
        NValue value = NValue::getNullValue(m_schema->columnType(ii));
        setNValue(ii, value);
    }
}

inline int TableTuple::compare(const TableTuple &other) const {
    const int columnCount = m_schema->columnCount();
    int diff;
    for (int ii = 0; ii < columnCount; ii++) {
        const NValue lhs = getNValue(ii);
        const NValue rhs = other.getNValue(ii);
        diff = lhs.compare(rhs);
        if (diff) {
            return diff;
        }
    }
    return 0;
}

inline size_t TableTuple::hashCode(size_t seed) const {
    const int columnCount = m_schema->columnCount();
    for (int i = 0; i < columnCount; i++) {
        const NValue value = getNValue(i);
        value.hashCombine(seed);
    }
    return seed;
}

inline size_t TableTuple::hashCode() const {
    size_t seed = 0;
    return hashCode(seed);
}

/**
 * Release to the heap any memory allocated for any uninlined columns.
 */
inline void TableTuple::freeObjectColumns() {
    const uint16_t unlinlinedColumnCount = m_schema->getUninlinedObjectColumnCount();
    std::vector<char*> oldObjects;
    for (int ii = 0; ii < unlinlinedColumnCount; ii++) {
        char** dataPtr = reinterpret_cast<char**>(getDataPtr(m_schema->getUninlinedObjectColumnInfoIndex(ii)));
        oldObjects.push_back(*dataPtr);
    }
    NValue::freeObjectsFromTupleStorage(oldObjects);
}

/**
 * Hasher for use with boost::unordered_map and similar
 */
struct TableTupleHasher : std::unary_function<TableTuple, std::size_t>
{
    /** Generate a 64-bit number for the key value */
    inline size_t operator()(TableTuple tuple) const
    {
        return tuple.hashCode();
    }
};

/**
 * Equality operator for use with boost::unrodered_map and similar
 */
class TableTupleEqualityChecker {
public:
    inline bool operator()(const TableTuple lhs, const TableTuple rhs) const {
        return lhs.equalsNoSchemaCheck(rhs);
    }
};

}

#endif
