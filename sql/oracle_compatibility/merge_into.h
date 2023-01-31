#ifndef MERGE_INTO_H
#define MERGE_INTO_H

#include "sql/parse_tree_nodes.h"

#include <stddef.h>
#include <sys/types.h>

#include "my_base.h"
#include "my_sqlcommand.h"
#include "sql/query_result.h"  // Query_result_interceptor
#include "sql/sql_cmd_dml.h"   // Sql_cmd_dml
#include "sql/sql_list.h"
#include "sql/item_cmpfunc.h"
#include "zsql_features.h"

class COPY_INFO;
class Copy_field;
class Item;
class SELECT_LEX_UNIT;
class THD;
class Temp_table_param;
struct TABLE;
struct TABLE_LIST;
class Parse_tree_item;
class Unique_on_insert;

void set_merge_into_target_table_trg_map(SELECT_LEX *select_lex,
                                         uint8 new_trg_event_map);

struct Merge_fields_values {
public:
  PT_item_list *column_list;
  PT_item_list *value_list;
  Item *where_clause;
  ulonglong merge_option;
  Merge_fields_values(PT_item_list *column_list_arg,
                      PT_item_list *value_list_arg,
                      Item *opt_where_clause_arg,
                      ulonglong merge_option_arg) {
    column_list = column_list_arg;
    value_list = value_list_arg;
    where_clause = opt_where_clause_arg;
    merge_option = merge_option_arg;
  }
};

/**
 * Reference class: PTI_context
 * PTI_context has specific context once created.
 *
 * For INSERT/UPDATE subclauses in MERGE INTO, the context of parse_placing
 * are not clear when cteating the PTI_context.
 * Thus, create a new PTI class which is similar to PTI_context but do not assign
 * context when creating. Using the context assigned to parse_placing when
 * itemize of PTI_nonspecific_context is called.
 */
class PTI_nonspecific_context : public Parse_tree_item {
  typedef Parse_tree_item super;

  Item *expr;

 public:
  PTI_nonspecific_context(const POS &pos, Item *expr_arg) : super(pos), expr(expr_arg) {}

  bool itemize(Parse_context *pc, Item **res) override {
    if (super::itemize(pc, res)) return true;

    if (expr->itemize(pc, &expr)) return true;

    if (!expr->is_bool_func()) {
      expr = make_condition(pc, expr);
      if (expr == nullptr) return true;
    }

    DBUG_ASSERT(expr != NULL);
    expr->apply_is_true();

    *res = expr;
    return false;
  }
};

class PT_merge final : public Parse_tree_root {
  //PT_hint_list *opt_hints{nullptr};
  PT_joined_table_on *m_join_table;

//insert
  PT_item_list *m_insert_column_list{nullptr};
  PT_item_list *m_insert_value_list{nullptr};
  Item *opt_where_clause_insert{nullptr};

//update
  PT_item_list *m_update_column_list{nullptr};
  PT_item_list *m_update_value_list{nullptr};
  Item *opt_where_clause_update{nullptr};

  ulonglong merge_option;

 public:
  PT_merge(PT_joined_table_on *join_table_arg,
           Merge_fields_values *insert_arg,
           Merge_fields_values *update_arg,
           ulonglong merge_option_arg)
      : m_join_table(join_table_arg),
        merge_option(merge_option_arg) {
          if (merge_option & ORA_MERGE_INTO_WITH_INSERT) {
            m_insert_column_list = insert_arg->column_list;
            m_insert_value_list = insert_arg->value_list;
            opt_where_clause_insert = insert_arg->where_clause;
          }
          if (merge_option & ORA_MERGE_INTO_WITH_UPDATE) {
            m_update_column_list = update_arg->column_list;
            m_update_value_list = update_arg->value_list;
            opt_where_clause_update = update_arg->where_clause;
          }
        }

  Sql_cmd *make_cmd(THD *thd) override;

 private:
  bool contextualize_insert_body(THD *thd, Parse_context *pc,
                                 SELECT_LEX *const select);
  bool contextualize_update_body(THD *, Parse_context *pc,
                                 SELECT_LEX *const select);
};

class Query_result_merge final : public Query_result_interceptor {
  /// merge target table
  TABLE_LIST *table_list;
  TABLE *table{nullptr};

  List<Item> *insert_columns;
  List<Item> *insert_values;
  Item *opt_where_clause_insert;

  List<Item> *update_fields;
  List<Item> *update_values;
  Item *opt_where_clause_update;

 public:
  ulonglong merge_option;
  /**
     error handling (rollback and binlogging) can happen in send_eof()
     so that afterward send_error() needs to find out that.
  */
  //bool error_handled;

 private:
  // tmp duplication weedout table to store rowid of modified records
  Unique_on_insert *m_unique;
  //MY_BITMAP saved_parts_map;
  //bool parts_map_has_initialized;
  // mark target table is empty table and all ops are insert
  bool all_ops_insert{false};

 protected:
  /// ha_start_bulk_insert has been called. Never cleared.
  bool bulk_insert_started;

 public:
  ulonglong autoinc_value_of_last_inserted_row;  // autogenerated or not
  COPY_INFO info;
  COPY_INFO update;  ///< the UPDATE part of "info"

  ulonglong found_rows;
  ulonglong updated_rows;

 public:
  Query_result_merge(TABLE_LIST *table_list_arg,
                      List<Item> *insert_columns_arg,List<Item> *insert_values_arg,
                      Item *where_clause_insert_arg,
                      List<Item> *update_fields_arg, List<Item> *update_values_arg,
                      Item *where_clause_update_arg,
                      ulonglong merge_option_arg)
      : Query_result_interceptor(),
        table_list(table_list_arg),
        insert_columns(insert_columns_arg),
        insert_values(insert_values_arg),
        opt_where_clause_insert(where_clause_insert_arg),
        update_fields(update_fields_arg),
        update_values(update_values_arg),
        opt_where_clause_update(where_clause_update_arg),
        merge_option(merge_option_arg),
        m_unique(nullptr),
        //parts_map_has_initialized(false),
        bulk_insert_started(false),
        autoinc_value_of_last_inserted_row(0),
        info(COPY_INFO::INSERT_OPERATION, insert_columns,
             // manage_defaults
             ((insert_columns && insert_columns->elements != 0) ||
             (insert_values && insert_values->elements == 0)),
             DUP_ERROR),
        update(COPY_INFO::UPDATE_OPERATION, update_fields, update_values),
        found_rows(0),
        updated_rows(0) {}

  bool need_explain_interceptor() const override { return true; }
  bool prepare(THD *, List<Item> &, SELECT_LEX_UNIT *) override;
  bool optimize() override;
  bool start_execution(THD *) override;
  bool send_data(THD *thd, List<Item> &) override;
  void send_error(THD *thd, uint errcode, const char *err) override;
  bool send_eof(THD *thd) override;
  void abort_result_set(THD *thd) override;
  void cleanup(THD *thd) override;
  bool no_merge_result_attached() override;

  bool stmt_binlog_is_trans() const {
    return table->file->has_transactions();
  }
  Item *get_insert_where() {return opt_where_clause_insert;}
  Item *get_update_where() {return opt_where_clause_update;}

 private:
  bool prepare_copy_info(TABLE *table);
  bool store_update_values(THD *thd, bool &is_row_changed);
  bool store_insert_values(THD *thd);
  bool filter_solved_record();
  bool check_solved_record();
  bool update_data(THD *thd);
  bool insert_data(THD *thd);
  bool initial_unique_table();
};

class Sql_cmd_merge final : public Sql_cmd_dml {
 public:
  Sql_cmd_merge(ulonglong merge_option_arg,
                List<Item> *insert_column,
                List<Item> *insert_value,
                Item *where_insert,
                List<Item> *update_column,
                List<Item> *update_value,
                Item *where_update)
      : merge_option(merge_option_arg),
        insert_column_list(insert_column),
        insert_value_list(insert_value),
        opt_where_clause_insert(where_insert),
        update_column_list(update_column),
        update_value_list(update_value),
        opt_where_clause_update(where_update) {}

  Sql_cmd_merge(ulonglong merge_option_arg,
                PT_item_list *insert_column,
                PT_item_list *insert_value,
                Item *where_insert,
                PT_item_list *update_column,
                PT_item_list *update_value,
                Item *where_update)
      : merge_option(merge_option_arg),
        insert_column_list(nullptr),
        insert_value_list(nullptr),
        opt_where_clause_insert(where_insert),
        update_column_list(nullptr),
        update_value_list(nullptr),
        opt_where_clause_update(where_update) {
          insert_column_list = insert_column ? &insert_column->value : nullptr;
          insert_value_list  = insert_value ? &insert_value->value : nullptr;
          update_column_list = update_column ? &update_column->value : nullptr;
          update_value_list  = update_value ? &update_value->value : nullptr;
        }

  enum_sql_command sql_command_code() const override {
    return SQLCOM_MERGE_INTO;
  }

 protected:
  bool precheck(THD *thd) override;

  bool prepare_inner(THD *thd) override;

 private:

  bool accept(THD *thd, Select_lex_visitor *visitor) override;

  bool prepare_update_insert_clause(THD *thd, SELECT_LEX *select,
                                    TABLE_LIST *table_list, TABLE *table);
  bool prepare_update_clause(THD *thd, SELECT_LEX *select, Name_resolution_context *ctx,
                             TABLE_LIST *table_list, TABLE *table);
  bool prepare_insert_clause(THD *thd, SELECT_LEX *select, Name_resolution_context *ctx,
                             TABLE_LIST *table_list, TABLE *table);
  bool check_insert_values_count(TABLE *table);
  bool setup_insert_clause(THD *thd, SELECT_LEX *select, TABLE_LIST *table_list);
  bool setup_update_clause(THD *thd, SELECT_LEX *select, TABLE_LIST *table_list);
  void push_down_where_to_select(SELECT_LEX *select);

 public:

  ulonglong merge_option;

  //insert
  List<Item> *insert_column_list;
  List<Item> *insert_value_list;
  Item *opt_where_clause_insert;

//update
  List<Item> *update_column_list;
  List<Item> *update_value_list;
  Item *opt_where_clause_update;
};

#endif // MERGE_INTO_H
