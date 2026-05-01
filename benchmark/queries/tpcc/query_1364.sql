-- {"operators": "CROSS_JOIN,AGGREGATE,TABLE_FUNCTION,SUBQUERY,UNNEST", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER", "non_incr_reason": "op:UNNEST,VALUES_ONLY"}
SELECT t.t AS tier, COUNT(*) FROM (SELECT unnest(['premium', 'standard', 'basic']) AS t) t CROSS JOIN CUSTOMER c GROUP BY t.t;
