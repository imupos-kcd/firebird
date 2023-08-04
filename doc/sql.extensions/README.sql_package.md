# RDB$SQL package (FB 6.0)

`RDB$SQL` is a package with utility routines to work with dynamic SQL.

## Procedure `EXPLAIN`

`RDB$SQL.EXPLAIN` returns tabular information of a query's plan, without execute the query.

Input parameters:
- `SQL` type `BLOB SUB_TYPE TEXT CHARACTER SET UTF8 NOT NULL` - query statement

Output parameters:
- `PLAN_LINE` type `INTEGER NOT NULL` - plan's line order
- `RECORD_SOURCE_ID` type `BIGINT NOT NULL` - record source id
- `PARENT_RECORD_SOURCE_ID` type `BIGINT` - parent record source id
- `LEVEL` type `INTEGER NOT NULL` - indentation level (may have gaps in relation to parent's level)
- `PACKAGE_NAME` type `RDB$PACKAGE_NAME` - package name of a stored procedure
- `OBJECT_NAME` type `RDB$RELATION_NAME` - object (table, procedure) name
- `ALIAS` type `RDB$RELATION_NAME` - alias name
- `RECORD_LENGTH` type `INTEGER` - record length for the record source
- `KEY_LENGTH` type `INTEGER` - key length for the record source
- `ACCESS_PATH` type `VARCHAR(255) CHARACTER SET UTF8 NOT NULL` - friendly plan description

```
select *
  from rdb$sql.explain('select * from employees where id = ?');
```

```
select *
  from rdb$sql.explain(q'{
    select *
    from (
      select name from employees
      union all
      select name from customers
    )
    where name = ?
  }');
```

# Authors
- Adriano dos Santos Fernandes
