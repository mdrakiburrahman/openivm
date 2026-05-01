-- {"operators": "UNNEST,TABLE_FUNCTION,LEFT_JOIN", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK", "non_incr_reason": "op:UNNEST"}
SELECT i.I_ID, u.slot, s.S_QUANTITY FROM ITEM i CROSS JOIN UNNEST([1, 2, 3]) AS u(slot) LEFT JOIN STOCK s ON s.S_I_ID = i.I_ID AND s.S_W_ID = u.slot;
