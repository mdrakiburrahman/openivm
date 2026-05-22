-- {"operators": "TABLE_FUNCTION,VALUES_ONLY,UNNEST", "complexity": "medium", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "UNKNOWN"}
SELECT unnest(['BC', 'GC']) AS credit_type;
