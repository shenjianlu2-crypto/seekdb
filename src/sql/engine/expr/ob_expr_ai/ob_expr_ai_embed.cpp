/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

namespace
{

// Argument positions of ai_embed(model_id, content[, dimension]).
const int64_t ARG_MODEL_IDX = 0;
const int64_t ARG_CONTENT_IDX = 1;
const int64_t ARG_DIM_IDX = 2;

struct EmbedRow
{
  EmbedRow()
    : model_datum_(nullptr), content_datum_(nullptr), dim_datum_(nullptr),
      res_datum_(nullptr), dim_(0), batch_idx_(-1), valid_(false)
  {}
  ObDatum *model_datum_;
  ObDatum *content_datum_;
  ObDatum *dim_datum_;
  ObDatum *res_datum_;
  ObString model_id_;
  ObString content_;
  int64_t dim_;
  int64_t batch_idx_;
  bool valid_;
};

int set_embed_row_result(const ObExpr &expr, ObEvalCtx &ctx, EmbedRow &row, ObString &result)
{
  int ret = OB_SUCCESS;
  if (row.batch_idx_ < 0) {
    if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, *row.res_datum_, result))) {
    }
  } else {
    char *res_buf = expr.get_str_res_mem(ctx, result.length(), row.batch_idx_);
    if (OB_ISNULL(res_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate embedding result buffer", K(ret));
    } else {
      if (result.length() > 0) {
        MEMCPY(res_buf, result.ptr(), result.length());
      }
      row.res_datum_->set_string(res_buf, result.length());
    }
  }
  return ret;
}

int embed_one_group(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &alloc,
                    ObArray<EmbedRow> &rows, const int64_t begin, const int64_t end)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, rows.at(begin).model_id_, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 rows.at(begin).model_id_, alloc, resolved_endpoint))) {
  } else {
    const int64_t dim = rows.at(begin).dim_;
    ObJsonObject *config = nullptr;
    ObJsonInt *dim_json = nullptr;
    if (dim > 0) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(alloc, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(alloc, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObArray<ObString> contents;
      ObArray<ObString> results;
      for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
        if (OB_FAIL(contents.push_back(rows.at(i).content_))) {
        }
      }
      ObAIFuncModel model(alloc, *info, resolved_endpoint);
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
      } else if (results.count() != contents.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected embedding result count", K(ret), K(results.count()), K(contents.count()));
      } else {
        for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
          ObString result = results.at(i - begin);
          if (OB_FAIL(set_embed_row_result(expr, ctx, rows.at(i), result))) {
          }
        }
      }
    }
  }
  return ret;
}

int eval_embed_rows(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &alloc,
                    ObArray<EmbedRow> &rows)
{
  int ret = OB_SUCCESS;
  // rows with null/empty model or content never enter a request, so one request
  // only carries the rows that it actually produces results for.
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    EmbedRow &row = rows.at(i);
    row.valid_ = false;
    row.dim_ = 0;
    if (OB_ISNULL(row.model_datum_) || row.model_datum_->is_null()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    } else if (OB_FALSE_IT(row.model_id_ = row.model_datum_->get_string())) {
    } else if (row.model_id_.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(row.content_datum_) || row.content_datum_->is_null()) {
        row.res_datum_->set_null();
        continue;
      } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                     ctx.exec_ctx_, alloc, *row.content_datum_,
                     expr.args_[ARG_CONTENT_IDX]->datum_meta_,
                     expr.args_[ARG_CONTENT_IDX]->obj_meta_.has_lob_header(),
                     row.content_))) {
      } else if (row.content_.empty()) {
        row.res_datum_->set_null();
        continue;
      }
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(row.dim_datum_) && !row.dim_datum_->is_null()) {
      row.dim_ = row.dim_datum_->get_int();
      if (row.dim_ <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
      }
    }
    if (OB_SUCC(ret)) {
      row.valid_ = true;
    }
  }

  int64_t begin = 0;
  while (OB_SUCC(ret) && begin < rows.count()) {
    if (!rows.at(begin).valid_) {
      ++begin;
      continue;
    }
    int64_t end = begin + 1;
    while (end < rows.count() && rows.at(end).valid_
           && rows.at(end).dim_ == rows.at(begin).dim_
           && 0 == rows.at(end).model_id_.compare(rows.at(begin).model_id_)) {
      ++end;
    }
    if (OB_FAIL(embed_one_group(expr, ctx, alloc, rows, begin, end))) {
    } else {
      begin = end;
    }
  }
  return ret;
}

} // anonymous namespace

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (3 == expr.arg_cnt_ ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObArray<EmbedRow> rows;
    EmbedRow row;
    row.model_datum_ = arg_model_id;
    row.content_datum_ = arg_content;
    row.dim_datum_ = arg_dim;
    row.res_datum_ = &res;
    row.batch_idx_ = -1;
    if (OB_FAIL(rows.push_back(row))) {
    } else if (OB_FAIL(eval_embed_rows(expr, ctx, temp_allocator, rows))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatumVector res_datum = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (3 == expr.arg_cnt_ && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObDatumVector model_array = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_array = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dim_array;
    if (3 == expr.arg_cnt_) {
      dim_array = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
    }
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObArray<EmbedRow> rows;
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; ++j) {
      if (skip.at(j) || eval_flags.at(j)) {
        continue;
      }
      EmbedRow row;
      row.model_datum_ = model_array.at(j);
      row.content_datum_ = content_array.at(j);
      row.dim_datum_ = 3 == expr.arg_cnt_ ? dim_array.at(j) : nullptr;
      row.res_datum_ = res_datum.at(j);
      row.batch_idx_ = j;
      if (OB_FAIL(rows.push_back(row))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(eval_embed_rows(expr, ctx, temp_allocator, rows))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      eval_flags.set(rows.at(i).batch_idx_);
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
