-- {"operators": "FILTER,ANTI_JOIN,SUBQUERY", "complexity": "medium", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK", "openivm_verified": true}
SELECT i.I_ID, i.I_NAME FROM ITEM i WHERE NOT EXISTS (SELECT 1 FROM STOCK s WHERE s.S_I_ID = i.I_ID AND s.S_QUANTITY > 0);
