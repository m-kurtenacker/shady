#include "shady/ir.h"

#include "log.h"
#include "portability.h"
#include "../type.h"
#include "../rewrite.h"

#include "list.h"

#include "dict.h"

#include <assert.h>

typedef struct Context_ {
    Rewriter rewriter;
    bool disable_lowering;
} Context;

static const Node* process_node(Context* ctx, const Node* node);

static const Node* process_body(Context* ctx, const Node* node, size_t start, const Node* outer_join, const Node* outer_reconvergence_token) {
    assert(node->tag == Body_TAG);
    IrArena* dst_arena = ctx->rewriter.dst_arena;

    const Body* old_body = &node->payload.body;
    struct List* accumulator = new_list(const Node*);
    // TODO add a @Synthetic annotation to tag those
    Nodes annotations = nodes(dst_arena, 0, NULL);

    assert(start <= old_body->instructions.count);
    for (size_t i = start; i < old_body->instructions.count; i++) {
        const Node* let_node = old_body->instructions.nodes[i];
        const Node* instr = let_node->tag == Let_TAG ? let_node->payload.let.instruction : let_node;

        switch (instr->tag) {
            case If_TAG: {
                // TODO handle yield types !
                bool has_false_branch = instr->payload.if_instr.if_false;
                Nodes yield_types = instr->payload.if_instr.yield_types;

                LARRAY(const Node*, rest_params, yield_types.count);
                for (size_t j = 0; j < yield_types.count; j++) {
                    rest_params[j] = let_node->payload.let.variables.nodes[j];
                }

                const Node* let_mask = let(dst_arena, prim_op(dst_arena, (PrimOp) {
                    .op = subgroup_active_mask_op,
                    .operands = nodes(dst_arena, 0, NULL)
                }), 1, NULL);
                append_list(const Node*, accumulator, let_mask);
                // reconvergence_token = NULL;

                Node* join_cont = fn(dst_arena, annotations, unique_name(dst_arena, "if_join"), true, nodes(dst_arena, yield_types.count, rest_params), nodes(dst_arena, 0, NULL));
                Node* true_branch = fn(dst_arena, annotations, unique_name(dst_arena, "if_true"), true, nodes(dst_arena, 0, NULL), nodes(dst_arena, 0, NULL));
                Node* false_branch = has_false_branch ? fn(dst_arena, annotations, unique_name(dst_arena, "if_false"), true, nodes(dst_arena, 0, NULL), nodes(dst_arena, 0, NULL)) : NULL;

                true_branch->payload.fn.body = process_body(ctx,  instr->payload.if_instr.if_true, 0, join_cont, let_mask->payload.let.variables.nodes[0]);
                if (has_false_branch)
                    false_branch->payload.fn.body = process_body(ctx,  instr->payload.if_instr.if_false, 0, join_cont, let_mask->payload.let.variables.nodes[0]);
                join_cont->payload.fn.body = process_body(ctx, node, i + 1, outer_join, reconvergence_token);

                Nodes instructions = nodes(dst_arena, entries_count_list(accumulator), read_list(const Node*, accumulator));
                destroy_list(accumulator);
                const Node* branch_t = branch(dst_arena, (Branch) {
                    .branch_mode = BrIfElse,
                    .branch_condition = rewrite_node(&ctx->rewriter, instr->payload.if_instr.condition),
                    .true_target = true_branch,
                    .false_target = has_false_branch ? false_branch : join_cont,
                    .args = nodes(dst_arena, 0, NULL),
                });
                return body(dst_arena, (Body) {
                    .instructions = instructions,
                    .terminator = branch_t
                });
            }
            case Loop_TAG: error("TODO")
            case Call_TAG: {
                if (!instr->payload.call_instr.is_indirect)
                    goto recreate_identity;

                Nodes cont_params = recreate_variables(&ctx->rewriter, let_node->payload.let.variables);
                for (size_t j = 0; j < cont_params.count; j++)
                    register_processed(&ctx->rewriter, let_node->payload.let.variables.nodes[j], cont_params.nodes[j]);

                Node* return_continuation = fn(dst_arena, annotations, unique_name(dst_arena, "call_continue"), true, cont_params, nodes(dst_arena, 0, NULL));
                return_continuation->payload.fn.body = process_body(ctx, node, i + 1, outer_join, reconvergence_token);

                Nodes instructions = nodes(dst_arena, entries_count_list(accumulator), read_list(const Node*, accumulator));
                destroy_list(accumulator);

                // TODO we probably want to emit a callc here and lower that later to a separate function in an optional pass
                return body(dst_arena, (Body) {
                    .instructions = instructions,
                    .terminator = callc(dst_arena, (Callc) {
                        .join_at = return_continuation,
                        .callee = process_node(ctx, instr->payload.call_instr.callee),
                        .args = rewrite_nodes(&ctx->rewriter, instr->payload.call_instr.args)
                    })
                });
            }
            default: break;
        }

        recreate_identity: {
            const Node* imported = recreate_node_identity(&ctx->rewriter, let_node);
            append_list(const Node*, accumulator, imported);
        }
    }

    const Node* old_terminator = old_body->terminator;
    const Node* new_terminator = NULL;

    switch (old_terminator->tag) {
        case MergeConstruct_TAG: {
            switch (old_terminator->payload.merge_construct.construct) {
                case Selection: {
                    assert(outer_join);
                    new_terminator = join(dst_arena, (Join) {
                        .is_indirect = false,
                        .join_at = outer_join,
                        .args = nodes(dst_arena, old_terminator->payload.merge_construct.args.count, old_terminator->payload.merge_construct.args.nodes),
                        .desired_mask = reconvergence_token
                    });
                    break;
                }
                // TODO handle other kind of merges
                case Continue:
                case Break: error("TODO")
                default: SHADY_UNREACHABLE;
            }
            break;
        }
        default: new_terminator = recreate_node_identity(&ctx->rewriter, old_terminator); break;
    }

    assert(new_terminator);
    Nodes instructions = nodes(dst_arena, entries_count_list(accumulator), read_list(const Node*, accumulator));
    destroy_list(accumulator);
    return body(dst_arena, (Body) {
        .instructions = instructions,
        .terminator = new_terminator
    });
}

static const Node* process_node(Context* ctx, const Node* node) {
    const Node* already_done = search_processed(&ctx->rewriter, node);
    if (already_done)
        return already_done;

    switch (node->tag) {
        case Function_TAG: {
            Node* fun = recreate_decl_header_identity(&ctx->rewriter, node);
            Context sub_ctx = *ctx;
            sub_ctx.disable_lowering = lookup_annotation_with_string_payload(fun, "DisablePass", "lower_cf_instrs");
            fun->payload.fn.body = process_node(&sub_ctx, node->payload.fn.body);
            return fun;
        }
        case Body_TAG: return ctx->disable_lowering ? recreate_node_identity(&ctx->rewriter, node) : process_body(ctx, node, 0, NULL, NULL);
        default: return recreate_node_identity(&ctx->rewriter, node);
    }
}

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

const Node* lower_cf_instrs(SHADY_UNUSED CompilerConfig* config, IrArena* src_arena, IrArena* dst_arena, const Node* src_program) {
    struct Dict* done = new_dict(const Node*, Node*, (HashFn) hash_node, (CmpFn) compare_node);
    Context ctx = {
        .rewriter = {
            .dst_arena = dst_arena,
            .src_arena = src_arena,
            .rewrite_fn = (RewriteFn) process_node,
            .processed = done,
        },
    };

    assert(src_program->tag == Root_TAG);

    const Node* rewritten = recreate_node_identity(&ctx.rewriter, src_program);

    destroy_dict(done);
    return rewritten;
}
