#include "local_alignment.h"
#include "utf8/utf8.h"


static alignment_ops_t NULL_ALIGNMENT_OPS = {
    .num_matches = 0,
    .num_mismatches = 0,
    .num_transpositions = 0,
    .num_gap_opens = 0,
    .num_gap_extensions = 0
};

static inline bool utf8_is_non_character(int32_t c) {
    int cat = utf8proc_category(c);
    return utf8_is_whitespace(c) || utf8_is_hyphen(c) || utf8_is_punctuation(cat);
}

alignment_ops_t affine_gap_align_op_counts_unicode_options(uint32_array *u1_array, uint32_array *u2_array, alignment_options_t options) {
    if (u1_array->n < u2_array->n) {
        uint32_array *tmp_array = u1_array;
        u1_array = u2_array;
        u2_array = tmp_array;
    }

    size_t m = u1_array->n;
    size_t n = u2_array->n;

    uint32_t *u1 = u1_array->a;
    uint32_t *u2 = u2_array->a;

    alignment_ops_t edits = NULL_ALIGNMENT_OPS;

    if (unicode_equals(u1_array, u2_array)) {
        edits.num_matches = n;
        return edits;
    }

    size_t num_bytes = (m + 1) * sizeof(size_t);

    size_t *C = malloc(num_bytes);
    if (C == NULL) {
        return NULL_ALIGNMENT_OPS;
    }

    size_t *D = malloc(num_bytes);
    if (D == NULL) {
        free(C);
        return NULL_ALIGNMENT_OPS;
    }

    alignment_ops_t *E = malloc((m + 1) * sizeof(alignment_ops_t));
    if (E == NULL) {
        free(C);
        free(D);
        return NULL_ALIGNMENT_OPS;
    }

    alignment_ops_t *ED = malloc((m + 1) * sizeof(alignment_ops_t));
    if (ED == NULL) {
        free(C);
        free(D);
        free(E);
        return NULL_ALIGNMENT_OPS;
    }

    size_t gap_open_cost = options.gap_open_cost;
    size_t gap_extend_cost = options.gap_extend_cost;
    size_t match_cost = options.match_cost;
    size_t mismatch_cost = options.mismatch_cost;
    size_t transpose_cost = options.transpose_cost;

    size_t e = 0, c = 0, s = 0;

    C[0] = 0;
    E[0] = NULL_ALIGNMENT_OPS;
    size_t t = gap_open_cost;

    alignment_ops_t base_edits = NULL_ALIGNMENT_OPS;
    base_edits.num_gap_opens++;

    for (size_t j = 1; j < m + 1; j++) {
        t += gap_extend_cost;
        C[j] = t;
        D[j] = t + gap_open_cost;
        base_edits.num_gap_extensions++;
        E[j] = base_edits;
        ED[j] = base_edits;
    }

    t = gap_open_cost;
    base_edits = NULL_ALIGNMENT_OPS;
    base_edits.num_gap_opens++;

    alignment_ops_t current_edits = NULL_ALIGNMENT_OPS;
    alignment_ops_t prev_char_edits = NULL_ALIGNMENT_OPS;
    alignment_ops_t prev_row_prev_char_edits = NULL_ALIGNMENT_OPS;

    bool in_gap = false;

    for (size_t i = 1; i < n + 1; i++) {
        // s = CC[0]
        s = C[0];
        uint32_t c2 = u2[i - 1];
        // CC[0] = c = t = t + h
        t += gap_extend_cost;
        c = t;
        C[0] = c;
        
        prev_row_prev_char_edits = E[0];
        base_edits.num_gap_extensions++;
        prev_char_edits = base_edits;
        E[0] = prev_char_edits;

        // e = t + g
        e = t + gap_open_cost;

        alignment_op op = ALIGN_GAP_OPEN;

        ssize_t match_at = -1;

        size_t min_at = 0;
        size_t min_cost = SIZE_MAX;

        for (size_t j = 1; j < m + 1; j++) {
            // insertion
            // e = min(e, c + g) + h
            size_t min = e;
            uint32_t c1 = u1[j - 1];

            alignment_op insert_op = ALIGN_GAP_OPEN;

            if ((c + gap_open_cost) < min) {
                min = c + gap_open_cost;
                insert_op = ALIGN_GAP_OPEN;
            } else {
                insert_op = ALIGN_GAP_EXTEND;
            }

            e = min + gap_extend_cost;

            // deletion
            // DD[j] = min(DD[j], CC[j] + g) + h

            alignment_op delete_op = ALIGN_GAP_OPEN;

            min = D[j];
            alignment_ops_t delete_edits = ED[j];
            alignment_ops_t delete_edits_stored = delete_edits;
            delete_op = ALIGN_GAP_OPEN;
            if (C[j] + gap_open_cost < min) {
                min = C[j] + gap_open_cost;
                
                delete_edits = delete_edits_stored = E[j];
                delete_edits_stored.num_gap_opens++;
            }

            D[j] = min + gap_extend_cost;
            delete_edits_stored.num_gap_extensions++;
            ED[j] = delete_edits_stored;

            // Cost
            // c = min(DD[j], e, s + w(a, b))

            alignment_op current_op = delete_op;


            min = D[j];

            // Delete transition
            current_edits = delete_edits;

            if (e < min) {
                min = e;
                // Insert transition
                current_op = insert_op;
                current_edits = prev_char_edits;
            }

            bool both_non_characters = utf8_is_non_character((int32_t)c1) && utf8_is_non_character((int32_t)c2);

            bool is_transpose = false;
            size_t w = c1 != c2 && !both_non_characters ? mismatch_cost : match_cost;

            if (c1 != c2 && j < m && !both_non_characters && c2 == u1[j] && i < n && c1 == u2[i]) {
                w = transpose_cost;
                is_transpose = true;
            }

            if (s + w < min) {
                min = s + w;

                // Match/mismatch/transpose transition
                current_edits = prev_row_prev_char_edits;

                if ((c1 == c2 || both_non_characters) && !is_transpose) {
                    current_op = ALIGN_MATCH;
                } else if (!is_transpose) {
                    current_op = ALIGN_MISMATCH;
                } else if (is_transpose) {
                    current_op = ALIGN_TRANSPOSE;
                }
            }

            if (current_op == ALIGN_MATCH) {
                if (!both_non_characters) {
                    current_edits.num_matches++;
                }
            } else if (current_op == ALIGN_MISMATCH) {
                current_edits.num_mismatches++;
            } else if (current_op == ALIGN_GAP_EXTEND) {
                current_edits.num_gap_extensions++;
            } else if (current_op == ALIGN_GAP_OPEN) {
                current_edits.num_gap_opens++;
                current_edits.num_gap_extensions++;
            } else if (current_op == ALIGN_TRANSPOSE) {
                current_edits.num_transpositions++;
            }

            if (min < min_cost) {
                op = current_op;
                min_cost = min;
                min_at = j;
            }

            c = min;
            s = C[j];
            C[j] = c;

            prev_char_edits = current_edits;
            prev_row_prev_char_edits = E[j];
            E[j] = prev_char_edits;

            // In the case of a transposition, duplicate costs for next character and advance by 2
            if (current_op == ALIGN_TRANSPOSE) {
                E[j + 1] = E[j];
                C[j + 1] = C[j];
                j++;
            }
        }

        if (op == ALIGN_TRANSPOSE) {
            i++;
        }

    }

    edits = E[m];
    free(C);
    free(D);
    free(E);
    free(ED);

    return edits;

}

alignment_ops_t affine_gap_align_op_counts_unicode(uint32_array *u1_array, uint32_array *u2_array) {
    return affine_gap_align_op_counts_unicode_options(u1_array, u2_array, DEFAULT_ALIGNMENT_OPTIONS_AFFINE_GAP);
}

alignment_ops_t affine_gap_align_op_counts_options(const char *s1, const char *s2, alignment_options_t options) {
    if (s1 == NULL || s2 == NULL) return NULL_ALIGNMENT_OPS;

    uint32_array *u1_array = unicode_codepoints(s1);
    if (u1_array == NULL) return NULL_ALIGNMENT_OPS;

    uint32_array *u2_array = unicode_codepoints(s2);

    if (u2_array == NULL) {
        uint32_array_destroy(u1_array);
        return NULL_ALIGNMENT_OPS;
    }

    alignment_ops_t edits = affine_gap_align_op_counts_unicode_options(u1_array, u2_array, options);

    uint32_array_destroy(u1_array);
    uint32_array_destroy(u2_array);

    return edits;
}


alignment_ops_t affine_gap_align_op_counts(const char *s1, const char *s2) {
    return affine_gap_align_op_counts_options(s1, s2, DEFAULT_ALIGNMENT_OPTIONS_AFFINE_GAP);
}