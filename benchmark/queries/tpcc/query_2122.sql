-- {"operators": "POSITIONAL_JOIN", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ITEM"}
SELECT s.S_W_ID, s.S_I_ID, i.I_ID, i.I_NAME FROM STOCK s POSITIONAL JOIN ITEM i;
