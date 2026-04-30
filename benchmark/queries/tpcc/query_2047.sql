-- {"operators": "INTERSECT_ALL,WINDOW", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ORDER_LINE"}
SELECT S_W_ID, S_I_ID, ROW_NUMBER() OVER (PARTITION BY S_W_ID ORDER BY S_I_ID) AS rn FROM STOCK INTERSECT ALL SELECT OL_SUPPLY_W_ID, OL_I_ID, ROW_NUMBER() OVER (PARTITION BY OL_SUPPLY_W_ID ORDER BY OL_I_ID) FROM ORDER_LINE;
