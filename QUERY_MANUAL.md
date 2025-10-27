# Parameterized Query System

## Overview

This document describes the parameterized query system that provides a unified interface for executing various types of queries on compressed JSON data. The system allows users to run different query types with standardized parameters.

## Query Types and Parameters

The system supports the following query types:

### 1. Field Existence Query (--field)
Check if a field exists in the dataset.

**Parameters:**
- `FIELD_NAME` - Name of the field to check

**Example:**
```bash
./parameterized_query --data-dir compressed_data --field user
```

### 2. Dictionary Query (--dict)
Retrieve dictionary values for a field.

**Parameters:**
- `FIELD_NAME` - Name of the field to query

**Example:**
```bash
./parameterized_query --data-dir compressed_data --dict user
```

### 3. Exact Match Query (--point)
Find records with exact field values.

**Parameters:**
- `FIELD_NAME` - Name of the field to query
- `VALUE` - Value to match

**Example:**
```bash
./parameterized_query --data-dir compressed_data --point user postgres
```

### 4. Range Query (--range)
Find records within a value range.

**Parameters:**
- `FIELD_NAME` - Name of the field to query
- `LOW` - Lower bound of the range
- `HIGH` - Upper bound of the range

**Example:**
```bash
./parameterized_query --data-dir compressed_data --range pid 7880 7890
```

### 5. Aggregate Query (--aggregate)
Perform aggregation functions (COUNT, SUM, AVG, MAX, MIN).

**Parameters:**
- `FUNCTION` - Aggregation function (COUNT, SUM, AVG, MAX, MIN)
- `FIELD` - Field name (optional for COUNT(*))

**Examples:**
```bash
./parameterized_query --data-dir compressed_data --aggregate COUNT
./parameterized_query --data-dir compressed_data --aggregate SUM pid
```

### 6. Complex Query (--complex)
Execute complex queries with logical operations.

**Parameters:**
- `QUERY_EXPRESSION` - Complex query expression

**Examples:**
```bash
./parameterized_query --data-dir compressed_data --complex "user:postgres"
./parameterized_query --data-dir compressed_data --complex "user:postgres AND dbname:example"
./parameterized_query --data-dir compressed_data --complex "(user:postgres OR user:alice) AND dbname:example"
```

### 7. Group By Query (--group)
Group records by field values.

**Parameters:**
- `FIELD_NAMES...` - One or more field names to group by

**Examples:**
```bash
./parameterized_query --data-dir compressed_data --group user
./parameterized_query --data-dir compressed_data --group user dbname
```

## Data Directory

All queries require a data directory containing compressed data:

```bash
./parameterized_query --data-dir PATH_TO_COMPRESSED_DATA [QUERIES...]
```

## Multiple Queries

Multiple queries can be executed in sequence:

```bash
./parameterized_query --data-dir compressed_data --field user --point user postgres --aggregate COUNT
```

## Building and Running

1. **Build the test program:**
   ```bash
   mkdir build
   cd build
   cmake ..
   make parameterized_query
   ```

2. **Run the test program:**
   ```bash
   ./parameterized_query --data-dir PATH_TO_DATA [QUERIES...]
   ```

## Examples

Here are some complete examples:

```bash
# Check if 'user' field exists
./parameterized_query --data-dir compressed_type_aware_data --field user

# Get dictionary values for 'user' field
./parameterized_query --data-dir compressed_type_aware_data --dict user

# Find records where user is 'postgres'
./parameterized_query --data-dir compressed_type_aware_data --point user postgres

# Find records where pid is between 7880 and 7890
./parameterized_query --data-dir compressed_type_aware_data --range pid 7880 7890

# Count all records
./parameterized_query --data-dir compressed_type_aware_data --aggregate COUNT

# Sum all pid values
./parameterized_query --data-dir compressed_type_aware_data --aggregate SUM pid

# Complex query with logical operations
./parameterized_query --data-dir compressed_type_aware_data --complex "user:postgres AND dbname:example"

# Group by user field
./parameterized_query --data-dir compressed_type_aware_data --group user
```