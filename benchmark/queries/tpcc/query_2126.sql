-- {"operators": "POSITIONAL_JOIN,UNNEST,TABLE_FUNCTION", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ITEM"}
SELECT s.S_W_ID, i.I_ID, u.label FROM STOCK s POSITIONAL JOIN ITEM i CROSS JOIN UNNEST(['left_pos', 'right_pos']) AS u(label);
