/*
 * Copyright (C) 2021 Santiago León O.
 */

#include <limits.h>
#include "html_builder.h"

struct html_t* markup_to_html (mem_pool_t *pool, char *markup, char *id, int x);

#if defined(MARKUP_PARSER_IMPL)

//function UserTag (tag_name, callback, user_data) {
//    this.name = tag_name;
//    this.callback = callback;
//    this.user_data = user_data;
//}
//
//function new_user_tag (tag_name, callback)
//{
//    __g_user_tags[tag_name] = new UserTag(tag_name, callback);
//}

#define TOKEN_TYPES_TABLE                      \
    TOKEN_TYPES_ROW(TOKEN_TYPE_UNKNOWN)        \
    TOKEN_TYPES_ROW(TOKEN_TYPE_TITLE)          \
    TOKEN_TYPES_ROW(TOKEN_TYPE_PARAGRAPH)      \
    TOKEN_TYPES_ROW(TOKEN_TYPE_BULLET_LIST)    \
    TOKEN_TYPES_ROW(TOKEN_TYPE_NUMBERED_LIST)  \
    TOKEN_TYPES_ROW(TOKEN_TYPE_TAG)            \
    TOKEN_TYPES_ROW(TOKEN_TYPE_OPERATOR)       \
    TOKEN_TYPES_ROW(TOKEN_TYPE_SPACE)          \
    TOKEN_TYPES_ROW(TOKEN_TYPE_TEXT)           \
    TOKEN_TYPES_ROW(TOKEN_TYPE_CODE_HEADER)    \
    TOKEN_TYPES_ROW(TOKEN_TYPE_CODE_LINE)      \
    TOKEN_TYPES_ROW(TOKEN_TYPE_BLANK_LINE)     \
    TOKEN_TYPES_ROW(TOKEN_TYPE_END_OF_FILE)

#define TOKEN_TYPES_ROW(value) value,
enum psx_token_type_t {
    TOKEN_TYPES_TABLE
};
#undef TOKEN_TYPES_ROW

#define TOKEN_TYPES_ROW(value) #value,
char* psx_token_type_names[] = {
    TOKEN_TYPES_TABLE
};
#undef TOKEN_TYPES_ROW

typedef struct {
    char *s;
    uint32_t len;
} sstring_t;
#define SSTRING(s,len) ((sstring_t){s,len})

static inline
sstring_t sstr_set (char *s, uint32_t len)
{
    return SSTRING(s, len);
}

static inline
sstring_t sstr_trim (sstring_t str)
{
    while (is_space (str.s)) {
        str.s++;
        str.len--;
    }

    while (is_space (str.s + str.len - 1) || str.s[str.len - 1] == '\n') {
        str.len--;
    }

    return str;
}

static inline
void sstr_extend (sstring_t *str1, sstring_t *str2)
{
    assert (str1->s + str1->len == str2->s);
    str1->len += str2->len;
}


struct psx_token_t {
    sstring_t value;

    enum psx_token_type_t type;
    int margin;
    int content_start;
    int heading_number;
    bool is_eol;
};

#define BLOCK_TYPES_TABLE                 \
    BLOCK_TYPES_ROW(BLOCK_TYPE_ROOT)      \
    BLOCK_TYPES_ROW(BLOCK_TYPE_LIST)      \
    BLOCK_TYPES_ROW(BLOCK_TYPE_LIST_ITEM) \
    BLOCK_TYPES_ROW(BLOCK_TYPE_HEADING)   \
    BLOCK_TYPES_ROW(BLOCK_TYPE_PARAGRAPH) \
    BLOCK_TYPES_ROW(BLOCK_TYPE_CODE)

#define BLOCK_TYPES_ROW(value) value,
enum psx_block_type_t {
    BLOCK_TYPES_TABLE
};
#undef BLOCK_TYPES_ROW

#define BLOCK_TYPES_ROW(value) #value,
char* psx_block_type_names[] = {
    BLOCK_TYPES_TABLE
};
#undef BLOCK_TYPES_ROW

struct psx_block_t {
    enum psx_block_type_t type;
    int margin;

    string_t inline_content;

    int heading_number;

    // List
    enum psx_token_type_t list_type;

    // Number of characters from the start of a list marker to the start of the
    // content. Includes al characters of the list marker.
    // "    -    C" -> 5
    // "   10.   C" -> 6
    int content_start;

    // Each non-leaf block contains a linked list of children
    struct psx_block_t *block_content;
    struct psx_block_t *block_content_end;

    struct psx_block_t *next;
};

struct psx_parser_state_t {
    mem_pool_t pool;

    bool error;
    bool error_msg;
    bool is_eof;
    bool is_eol;
    bool is_peek;
    char *str;

    char *pos;
    char *pos_peek;

    struct psx_token_t token;
    struct psx_token_t token_peek;

    DYNAMIC_ARRAY_DEFINE (struct psx_block_t*, block_stack);
};

void ps_init (struct psx_parser_state_t *ps, char *str)
{
    ps->str = str;
    ps->pos = str;
    DYNAMIC_ARRAY_INIT (&ps->pool, ps->block_stack, 100);
}

void ps_destroy (struct psx_parser_state_t *ps)
{
    mem_pool_destroy (&ps->pool);
}

bool char_in_str (char c, char *str)
{
    while (*str != '\0') {
        if (*str == c) {
            return true;
        }

        str++;
    }

    return false;
}

static inline
char ps_curr_char (struct psx_parser_state_t *ps)
{
    return *(ps->pos);
}

bool pos_is_eof (struct psx_parser_state_t *ps)
{
    return ps_curr_char(ps) == '\0';
}

bool pos_is_operator (struct psx_parser_state_t *ps)
{
    return !pos_is_eof(ps) && char_in_str(ps_curr_char(ps), ",=[]{}\n\\");
}

bool pos_is_digit (struct psx_parser_state_t *ps)
{
    return !pos_is_eof(ps) && char_in_str(ps_curr_char(ps), "1234567890");
}

bool pos_is_space (struct psx_parser_state_t *ps)
{
    return !pos_is_eof(ps) && is_space(ps->pos);
}

bool is_empty_line (sstring_t line)
{
    int count = 0;
    while (is_space(line.s + count)) {
        count++;
    }

    return line.s[count] == '\n' || count == line.len;
}

static inline
void ps_advance_char (struct psx_parser_state_t *ps)
{
    if (!pos_is_eof(ps)) {
        ps->pos++;

    } else {
        ps->is_eof = true;
    }
}

// NOTE: c_str MUST be null terminated!!
bool ps_match_str(struct psx_parser_state_t *ps, char *c_str)
{
    char *backup_pos = ps->pos;

    bool found = true;
    while (*c_str) {
        if (ps_curr_char(ps) != *c_str) {
            found = false;
            break;
        }

        ps_advance_char (ps);
        c_str++;
    }

    if (!found) ps->pos = backup_pos;

    return found;
}

static inline
bool ps_match_digits (struct psx_parser_state_t *ps)
{
    bool found = false;
    if (pos_is_digit(ps)) {
        found = true;
        while (pos_is_digit(ps)) {
            ps_advance_char (ps);
        }
    }
    return found;
}

static inline
void ps_consume_spaces (struct psx_parser_state_t *ps)
{
    while (pos_is_space(ps)) {
        ps_advance_char (ps);
    }
}

sstring_t advance_line(struct psx_parser_state_t *ps)
{
    char *start = ps->pos;
    while (!pos_is_eof(ps) && ps_curr_char(ps) != '\n') {
        ps_advance_char (ps);
    }

    // Advance over the \n character. We leave \n characters because they are
    // handled by the inline parser. Sometimes they are ignored (between URLs),
    // and sometimes they are replaced to space (multiline paragraphs).
    ps_advance_char (ps);

    return SSTRING(start, ps->pos - start);
}

BINARY_TREE_NEW (sstring_map, sstring_t, sstring_t, strncmp(a.s, b.s, MIN(a.len, b.len)))

struct sstring_ll_l {
    sstring_t v;
    struct sstring_ll_l *next;
};

struct psx_tag_parameters_t {
    struct sstring_ll_l *positional;
    struct sstring_ll_l *positional_end;

    struct sstring_map_tree_t named;
};

void ps_parse_tag_parameters (struct psx_parser_state_t *ps, struct psx_tag_parameters_t *parameters)
{
    if (parameters != NULL) {
        parameters->named.pool = &ps->pool;
    }

    if (ps_curr_char(ps) == '[') {
        while (!ps->is_eof && ps_curr_char(ps) != ']') {
            ps_advance_char (ps);

            char *start = ps->pos;
            if (ps_curr_char(ps) != '\"') {
                // TODO: Allow quote strings for values. This is to allow
                // values containing "," or "]".
            }

            while (ps_curr_char(ps) != ',' &&
                   ps_curr_char(ps) != '=' &&
                   ps_curr_char(ps) != ']') {
                ps_advance_char (ps);
            }

            if (ps_curr_char(ps) == ',' || ps_curr_char(ps) == ']') {
                if (parameters != NULL) {
                    LINKED_LIST_APPEND_NEW (&ps->pool, struct sstring_ll_l, parameters->positional, new_param);
                    new_param->v = sstr_trim(SSTRING(start, ps->pos - start));
                }

            } else {
                sstring_t name = sstr_trim(SSTRING(start, ps->pos - start));

                // Advance over the '=' character
                ps_advance_char (ps);

                char *value_start = ps->pos;
                while (ps_curr_char(ps) != ',' &&
                       ps_curr_char(ps) != ']') {
                    ps_advance_char (ps);
                }

                sstring_t value = sstr_trim(SSTRING(value_start, ps->pos - value_start));

                if (parameters != NULL) {
                    sstring_map_tree_insert (&parameters->named, name, value);
                }
            }

            if (ps_curr_char(ps) != ']') {
                ps_advance_char (ps);
            }
        }

        ps_advance_char (ps);
    }
}

struct psx_token_t ps_next_peek(struct psx_parser_state_t *ps)
{
    struct psx_token_t _tok = {0};
    struct psx_token_t *tok = &_tok;

    char *backup_pos = ps->pos;
    ps_consume_spaces (ps);

    tok->type = TOKEN_TYPE_PARAGRAPH;
    char *non_space_pos = ps->pos;
    if (pos_is_eof(ps)) {
        ps->is_eof = true;
        ps->is_eol = true;
        tok->type = TOKEN_TYPE_END_OF_FILE;

    } else if (ps_match_str(ps, "- ") || ps_match_str(ps, "* ")) {
        ps_consume_spaces (ps);

        tok->type = TOKEN_TYPE_BULLET_LIST;
        tok->value = SSTRING(non_space_pos, 1);
        tok->margin = non_space_pos - backup_pos;
        tok->content_start = ps->pos - non_space_pos;

    } else if (ps_match_digits(ps)) {
        char *number_end = ps->pos;
        if (ps->pos - non_space_pos <= 9 && ps_match_str(ps, ". ")) {
            ps_consume_spaces (ps);

            tok->type = TOKEN_TYPE_NUMBERED_LIST;
            tok->value = SSTRING(non_space_pos, number_end - non_space_pos);
            tok->margin = non_space_pos - backup_pos;
            tok->content_start = ps->pos - non_space_pos;

        } else {
            // It wasn't a numbered list marker, restore position.
            ps->pos = backup_pos;
        }

    } else if (ps_match_str(ps, "#")) {
        int heading_number = 1;
        while (ps_curr_char(ps) == '#') {
            heading_number++;
            ps_advance_char (ps);
        }

        if (pos_is_space (ps) && heading_number <= 6) {
            ps_consume_spaces (ps);

            tok->value = sstr_trim(advance_line (ps));
            tok->is_eol = true;
            tok->margin = non_space_pos - backup_pos;
            tok->type = TOKEN_TYPE_TITLE;
            tok->heading_number = heading_number;
        }

    } else if (ps_match_str(ps, "\\code")) {
        ps_parse_tag_parameters(ps, NULL);

        if (!ps_match_str(ps, "{")) {
            tok->type = TOKEN_TYPE_CODE_HEADER;
            while (pos_is_space(ps) || ps_curr_char(ps) == '\n') {
                ps_advance_char (ps);
            }

        } else {
            // False alarm, this was an inline code block, restore position.
            ps->pos = backup_pos;
        }

    } else if (ps_match_str(ps, "|")) {
        tok->value = advance_line (ps);
        tok->is_eol = true;
        tok->type = TOKEN_TYPE_CODE_LINE;

    } else if (ps_match_str(ps, "\n")) {
        tok->type = TOKEN_TYPE_BLANK_LINE;
    }

    if (tok->type == TOKEN_TYPE_PARAGRAPH) {
        tok->value = sstr_trim(advance_line (ps));
        tok->is_eol = true;
    }


    // Because we are only peeking, restore the old position and store the
    // resulting one in a separate variable.
    assert (!ps->is_peek && "Trying to peek more than once in a row");
    ps->is_peek = true;
    ps->token_peek = *tok;
    ps->pos_peek = ps->pos;
    ps->pos = backup_pos;

    //printf ("%s: '%.*s'\n", psx_token_type_names[ps->token_peek.type], ps->token_peek.value.len, ps->token_peek.value.s);

    return ps->token_peek;
}

struct psx_token_t ps_next(struct psx_parser_state_t *ps) {
    if (!ps->is_peek) {
        ps_next_peek(ps);
    }

    ps->pos = ps->pos_peek;
    ps->token = ps->token_peek;
    ps->is_peek = false;

    //printf ("%s: '%.*s'\n", psx_token_type_names[ps->token.type], ps->token.value.len, ps->token.value.s);

    return ps->token;
}

bool ps_match(struct psx_parser_state_t *ps, enum psx_token_type_t type, char *value)
{
    bool match = false;

    if (type == ps->token.type) {
        if (value == NULL) {
            match = true;

        } else if (strncmp(ps->token.value.s, value, ps->token.value.len)) {
            match = true;
        }
    }

    return match;
}

//function ps_parse_tag (ps)
//{
//    let tag = {
//        attributes: null,
//        content: null,
//    };
//
//    let attributes = ps_parse_tag_parameters(ps);
//    let content = "";
//
//    ps_expect_content (ps, TOKEN_TYPE_OPERATOR, "{")
//    tok = ps_next_content(ps)
//    while (!ps.is_eof && !ps.error && !ps_match(ps, TOKEN_TYPE_OPERATOR, "}")) {
//        content += tok.value
//        tok = ps_next_content(ps)
//    }
//
//    if (!ps.error) {
//        tag.attributes = attributes;
//        tag.content = content;
//    }
//
//    return tag;
//}

//function ps_parse_tag_balanced_braces (ps)
//{
//    let tag = {
//        attributes: null,
//        content: null,
//    };
//
//    let attributes = ps_parse_tag_parameters(ps);
//    let content = parse_balanced_brace_block(ps)
//
//    if (!ps.error) {
//        tag.attributes = attributes;
//        tag.content = content;
//    }
//
//    return tag;
//}

//let ContextType = {
//    ROOT: 0,
//    HTML: 1,
//}

//function ParseContext (type, dom_element) {
//    this.type = type
//    this.dom_element = dom_element
//
//    this.list_type = null
//}

//function array_end(array)
//{
//    return array[array.length - 1]
//}

//function append_dom_element (context_stack, dom_element)
//{
//    let head_ctx = array_end(context_stack)
//    head_ctx.dom_element.appendChild(dom_element)
//}

//function html_escape (str)
//{
//    return str.replaceAll("<", "&lt;").replaceAll(">", "&gt;")
//}

//function push_new_html_context (context_stack, tag)
//{
//    struct html_element_t *dom_element = html_new_element (html, tag);
//    append_dom_element (context_stack, dom_element);
//
//    let new_context = new ParseContext(ContextType.HTML, dom_element);
//    context_stack.push(new_context);
//    return new_context;
//}

//function pop_context (context_stack)
//{
//    let curr_ctx = context_stack.pop()
//    return array_end(context_stack)
//}

//// This function returns the same string that was parsed as the current token.
//// It's used as fallback, for example in the case of ',' and '#' characters in
//// the middle of paragraphs, or unrecognized tag sequences.
////
//// TODO: I feel that using this for operators isn't nice, we should probably
//// have a stateful tokenizer that will consider ',' as part of a TEXT token
//// sometimes and as operators other times. It's probably best to just have an
//// attribute tokenizer/parser, then the inline parser only deals with tags.
//function ps_literal_token (ps)
//{
//    let res = ps.token.value != null ? ps.token.value : ""
//    if (ps_match (ps, TOKEN_TYPE_TAG, null)) {
//        res = "\\" + ps.token.value
//    }
//
//    return res
//}

//function ps_next_content(ps)
//{
//    let pos_is_operator = function(ps) {
//        return char_in_str(ps_curr_char(ps), ",=[]{}\\");
//    }
//
//    let pos_is_space = function(ps) {
//        return char_in_str(ps_curr_char(ps), " \t");
//    }
//
//    let pos_is_eof = function(ps) {
//        return ps.pos >= ps.str.length;
//    }
//
//    let tok = new Token();
//
//    if (pos_is_eof(ps)) {
//        ps.is_eof = true;
//
//    } else if (ps_curr_char(ps) == "\\") {
//        // :tag_parsing
//        ps.pos++;
//
//        let start = ps.pos;
//        while (!pos_is_eof(ps) && !pos_is_operator(ps) && !pos_is_space(ps)) {
//            ps.pos++;
//        }
//        tok.value = SSTRING(start, ps.pos - start);
//        tok.type = TOKEN_TYPE_TAG;
//
//    } else if (pos_is_operator(ps)) {
//        tok.type = TOKEN_TYPE_OPERATOR;
//        tok.value = ps_curr_char(ps);
//        ps.pos++;
//
//    } else if (pos_is_space(ps) || ps_curr_char(ps) == "\n") {
//        // This consumes consecutive spaces into a single one.
//        while (pos_is_space(ps) || ps_curr_char(ps) == "\n") {
//            ps.pos++;
//        }
//
//        tok.value = " ";
//        tok.type = TOKEN_TYPE_SPACE;
//
//    } else {
//        let start = ps.pos;
//        while (!pos_is_eof(ps) && !pos_is_operator(ps) && !pos_is_space(ps) && ps_curr_char(ps) != "\n") {
//            ps.pos++;
//        }
//
//        tok.value = SSTRING(start, ps.pos - start);
//        tok.type = TOKEN_TYPE_TEXT;
//    }
//
//    if (pos_is_eof(ps)) {
//        ps.is_eof = true;
//    }
//
//    ps.token = tok;
//
//    //console.log(psx_token_type_names[tok.type] + ": " + tok.value)
//    return tok;
//}

//function ps_expect_content(ps, type, value)
//{
//    ps_next_content (ps)
//    if (!ps_match(ps, type, value)) {
//        ps.error = true
//        if (ps.type != type) {
//            if (value == NULL) {
//                ps.error_msg = sprintf("Expected token of type %s, got '%s' of type %s.",
//                                       psx_token_type_names[type], ps.value, psx_token_type_names[ps.type]);
//            } else {
//                ps.error_msg = sprintf("Expected token '%s' of type %s, got '%s' of type %s.",
//                                       value, psx_token_type_names[type], ps.value, psx_token_type_names[ps.type]);
//            }
//
//        } else {
//            // Value didn't match.
//            ps.error_msg = sprintf("Expected '%s', got '%s'.", value, ps.value);
//        }
//    }
//}

//function parse_balanced_brace_block(ps)
//{
//    let content = NULL
//
//    if (ps_curr_char(ps) == "{") {
//        content = "";
//        let brace_level = 1;
//        ps_expect_content (ps, TOKEN_TYPE_OPERATOR, "{");
//
//        while (!ps.is_eof && brace_level != 0) {
//            ps_next_content (ps);
//            if (ps_match (ps, TOKEN_TYPE_OPERATOR, "{")) {
//                brace_level++;
//            } else if (ps_match (ps, TOKEN_TYPE_OPERATOR, "}")) {
//                brace_level--;
//            }
//
//            if (brace_level != 0) { // Avoid appending the closing }
//                content += html_escape(ps_literal_token(ps));
//            }
//        }
//    }
//
//    return content;
//}

//function compute_media_size (attributes, aspect_ratio, max_width)
//{
//    let a_width = undefined;
//    if (attributes.named.width != undefined) {
//        a_width = Number(attributes.named.width)
//    }
//
//    let a_height = undefined;
//    if (attributes.named.height != undefined) {
//        a_height = Number(attributes.named.height)
//    }
//
//
//    let width, height;
//    if (a_height == undefined &&
//        a_width != undefined && a_width <= max_width) {
//        width = a_width;
//        height = width/aspect_ratio;
//
//    } else if (a_width == undefined &&
//               a_height != undefined && a_height <= max_width/aspect_ratio) {
//        height = a_height;
//        width = height*aspect_ratio;
//
//    } else if (a_width != undefined && a_width <= max_width &&
//               a_height != undefined && a_height <= max_width/aspect_ratio) {
//        width = a_width;
//        height = a_height;
//
//    } else {
//        width = max_width;
//        height = width/aspect_ratio;
//    }
//
//    return [width, height];
//}

// This function parses the content of a block of text. The formatting is
// limited to tags that affect the formating inline. This parsing function
// will not add nested blocks like paragraphs, lists, code blocks etc.
//
// TODO: How do we handle the prescence of nested blocks here?, ignore them and
// print them or raise an error and stop parsing.
void block_content_parse_text (struct html_element_t *container, string_t *content)
{
    //let ps = new ParserState(content);

    //let context_stack = [new ParseContext(ContextType.ROOT, container)]
    //while (!ps.is_eof && !ps.error) {
    //    let tok = ps_next_content (ps);

    //    if (ps_match(ps, TOKEN_TYPE_TEXT, NULL) || ps_match(ps, TOKEN_TYPE_SPACE, NULL)) {
    //        let curr_ctx = array_end(context_stack);
    //        curr_ctx.dom_element.innerHTML += tok.value;

    //    } else if (ps_match(ps, TOKEN_TYPE_OPERATOR, "}") && array_end(context_stack).type != ContextType.ROOT) {
    //        pop_context (context_stack);

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "i") || ps_match(ps, TOKEN_TYPE_TAG, "b")) {
    //        let tag = ps.token.value;
    //        ps_expect_content (ps, TOKEN_TYPE_OPERATOR, "{");
    //        push_new_html_context (context_stack, tag)

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "link")) {
    //        let tag = ps_parse_tag (ps);

    //        // We parse the URL from the title starting at the end. I thinkg is
    //        // far less likely to have a non URL encoded > character in the
    //        // URL, than a user wanting to use > inside their title.
    //        //
    //        // TODO: Support another syntax for the rare case of a user that
    //        // wants a > character in a URL. Or make sure URLs are URL encoded
    //        // from the UI that will be used to edit this.
    //        let pos = tag.content.length - 1
    //        while (pos > 1 && tag.content.charAt(pos) != ">") {
    //            pos--
    //        }

    //        let url = tag.content
    //        let title = tag.content
    //        if (pos != 1 && tag.content.charAt(pos - 1) == "-") {
    //            pos--
    //            url = tag.content.substr(pos + 2).trim()
    //            title = tag.content.substr(0, pos).trim()
    //        }

    //        struct html_element_t *link_element = html_new_element (html, "a")
    //        link_element.setAttribute("href", url)
    //        link_element.setAttribute("target", "_blank")
    //        link_element.innerHTML = title

    //        append_dom_element(context_stack, link_element);

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "youtube")) {
    //        let tag = ps_parse_tag (ps);

    //        let video_id;
    //        let regex = /^.*(youtu.be\/|youtube(-nocookie)?.com\/(v\/|.*u\/\w\/|embed\/|.*v=))([\w-]{11}).*/
    //        var match = tag.content.match(regex);
    //        if (match) {
    //            video_id = match[4];
    //        } else {
    //            video_id = "";
    //        }

    //        // Assume 16:9 aspect ratio
    //        let size = compute_media_size(tag.attributes, 16/9, content_width - 30);

    //        struct html_element_t *dom_element = html_new_element (html, "iframe")
    //        dom_element.setAttribute("width", size[0]);
    //        dom_element.setAttribute("height", size[1]);
    //        dom_element.setAttribute("style", "margin: 0 auto; display: block;");
    //        dom_element.setAttribute("src", "https://www.youtube-nocookie.com/embed/" + video_id);
    //        dom_element.setAttribute("frameborder", "0");
    //        dom_element.setAttribute("allow", "accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture");
    //        dom_element.setAttribute("allowfullscreen", "");
    //        append_dom_element(context_stack, dom_element);

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "image")) {
    //        let tag = ps_parse_tag (ps);

    //        struct html_element_t *img_element = html_new_element (html, "img")
    //        img_element.setAttribute("src", "files/" + tag.content)
    //        img_element.setAttribute("width", content_width)
    //        append_dom_element(context_stack, img_element);

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "code")) {
    //        let attributes = ps_parse_tag_parameters(ps);
    //        // TODO: Actually do something with the passed language name

    //        let code_content = parse_balanced_brace_block(ps)
    //        if (code_content != NULL) {
    //            struct html_element_t *code_element = html_new_element (html, "code");
    //            code_element.classList.add("code-inline");
    //            code_element.innerHTML = code_content;

    //            append_dom_element(context_stack, code_element);
    //        }

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "note")) {
    //        let tag = ps_parse_tag (ps);
    //        let note_title = tag.content;

    //        struct html_element_t *link_element = html_new_element (html, "a")
    //        link_element.setAttribute("onclick", "return open_note('" + note_title_to_id[note_title] + "');")
    //        link_element.setAttribute("href", "#")
    //        link_element.classList.add("note-link")
    //        link_element.innerHTML = note_title

    //        append_dom_element(context_stack, link_element);

    //    } else if (ps_match(ps, TOKEN_TYPE_TAG, "html")) {
    //        // TODO: How can we support '}' characters here?. I don't thing
    //        // assuming there will be balanced braces is an option here, as it
    //        // is in the \code tag. We most likely will need to implement user
    //        // defined termintating strings.
    //        let tag = ps_parse_tag (ps);
    //        struct html_element_t *dummy_element = html_new_element (html, "span");
    //        dummy_element.innerHTML = tag.content;

    //        if (dummy_element.firstChild != NULL) {
    //            append_dom_element(context_stack, dummy_element.firstChild);
    //        } else {
    //            append_dom_element(context_stack, dummy_element);
    //        }

    //    } else {
    //        let head_ctx = array_end(context_stack);
    //        head_ctx.dom_element.innerHTML += ps_literal_token(ps);
    //    }
    //}

    //ps_destroy (ps);
}

struct psx_block_t* psx_leaf_block_new(mem_pool_t *pool, enum psx_block_type_t type, int margin, sstring_t inline_content)
{
    struct psx_block_t *new_block = mem_pool_push_struct (pool, struct psx_block_t);
    *new_block = ZERO_INIT (struct psx_block_t);
    strn_set_pooled (pool, &new_block->inline_content, inline_content.s, inline_content.len);
    new_block->type = type;
    new_block->margin = margin;

    return new_block;
}

struct psx_block_t* psx_container_block_new(mem_pool_t *pool, enum psx_block_type_t type, int margin)
{
    struct psx_block_t *new_block = mem_pool_push_struct (pool, struct psx_block_t);
    *new_block = ZERO_INIT (struct psx_block_t);
    new_block->type = type;
    new_block->margin = margin;

    return new_block;
}

struct psx_block_t* psx_push_block (struct psx_parser_state_t *ps, struct psx_block_t *new_block)
{
    struct psx_block_t *curr_block = DYNAMIC_ARRAY_GET_LAST(ps->block_stack);

    //console.assert (curr_block.block_content != NULL, "Pushing nested block to non-container block.");
    LINKED_LIST_APPEND (curr_block->block_content, new_block);

    DYNAMIC_ARRAY_APPEND (ps->block_stack, new_block);
    return new_block;
}

//// TODO: User callbacks will be modifying the tree. It's possible the user
//// messes up and for example adds a cycle into the tree, we should detect such
//// problem and avoid maybe later entering an infinite loop.
//function block_tree_user_callbacks (root, block=NULL, containing_array=NULL, index=-1)
//{
//    // Convenience so just a single parameter is required in the initial call
//    if (block == NULL) block = root;
//
//    if (block.block_content != NULL) {
//        block.block_content.forEach (function(sub_block, index) {
//            block_tree_user_callbacks (root, sub_block, block.block_content, index);
//        });
//
//    } else {
//        struct psx_parser_state_t _ps = {0};
//        struct psx_parser_state_t *ps = &_ps;
//        ps_init (block->inline_content);
//
//        let string_replacements = [];
//        while (!ps.is_eof && !ps.error) {
//            let start = ps.pos;
//            let tok = ps_next_content (ps);
//            if (ps_match(ps, TOKEN_TYPE_TAG, NULL) && __g_user_tags[tok.value] != undefined) {
//                let user_tag = __g_user_tags[tok.value];
//                let result = user_tag.callback (ps, root, block, user_tag.user_data);
//
//                if (result != NULL) {
//                    if (typeof(result) == "string") {
//                        string_replacements.push ([start, ps.pos, result])
//                    } else {
//                        containing_array.splice(index, 1, ...result);
//                        break;
//                    }
//                }
//            }
//        }
//
//        for (let i=string_replacements.length - 1; i >= 0; i--) {
//            let replacement = string_replacements [i];
//            block.inline_content = block.inline_content.substring(0, replacement[0]) + replacement[2] + block.inline_content.substring(replacement[1]);
//        }
//
//        // TODO: What will happen if a parse error occurs here?. We should print
//        // some details about which user function failed by checking if there
//        // is an error in ps.
//        ps_destroy (ps);
//    }
//}

void block_tree_to_html (struct html_t *html, struct psx_block_t *block,  struct html_element_t *parent)
{
    if (block->type == BLOCK_TYPE_PARAGRAPH) {
        struct html_element_t *new_dom_element = html_new_element (html, "p");
        html_element_append_child (html, parent, new_dom_element);
        block_content_parse_text (new_dom_element, &block->inline_content);

    } else if (block->type == BLOCK_TYPE_HEADING) {
        string_t buff = {0};
        str_set_printf (&buff, "h%i", block->heading_number);
        struct html_element_t *new_dom_element = html_new_element (html, str_data(&buff));
        str_free (&buff);

        html_element_append_child (html, parent, new_dom_element);
        block_content_parse_text (new_dom_element, &block->inline_content);

    } else if (block->type == BLOCK_TYPE_CODE) {
        struct html_element_t *pre_element = html_new_element (html, "pre");
        html_element_append_child (html, parent, pre_element);

        struct html_element_t *code_element = html_new_element (html, "code");
        html_element_class_add(html, code_element, "code-block");
        // If we use line numbers, this should be the padding WRT the
        // column containing the numbers.
        // code_dom.style.paddingLeft = "0.25em"
        html_element_attribute_set(html, code_element, "style", "display: table");

        str_cpy (&code_element->text, &block->inline_content);
        html_element_append_child (html, pre_element, code_element);

        // This is a hack. It's the only way I found to show tight, centered
        // code blocks if they are smaller than content_width and at the same
        // time show horizontal scrolling if they are wider.
        //
        // We need to do this because "display: table", disables scrolling, but
        // "display: block" sets width to be the maximum possible. Here we
        // first create the element with "display: table", compute its width,
        // then change it to "display: block" but if it was smaller than
        // content_width, we explicitly set its width.
        //let tight_width = code_element.scrollWidth;
        //code_element.style.display = "block";
        //if (tight_width < content_width) {
        //    code_element.style.width = tight_width - code_block_padding*2 + "px";
        //}

    } else if (block->type == BLOCK_TYPE_ROOT) {
        LINKED_LIST_FOR (struct psx_block_t*, sub_block, block->block_content) {
            block_tree_to_html(html, sub_block, parent);
        }

    } else if (block->type == BLOCK_TYPE_LIST) {
        struct html_element_t *new_dom_element = html_new_element (html, "ul");
        if (block->list_type == TOKEN_TYPE_NUMBERED_LIST) {
            new_dom_element = html_new_element (html, "ol");
        }
        html_element_append_child (html, parent, new_dom_element);

        LINKED_LIST_FOR (struct psx_block_t*, sub_block, block->block_content) {
            block_tree_to_html(html, sub_block, new_dom_element);
        }

    } else if (block->type == BLOCK_TYPE_LIST_ITEM) {
        struct html_element_t *new_dom_element = html_new_element (html, "li");
        html_element_append_child (html, parent, new_dom_element);

        LINKED_LIST_FOR (struct psx_block_t*, sub_block, block->block_content) {
            block_tree_to_html(html, sub_block, new_dom_element);
        }
    }
}

struct psx_block_t* parse_note_text(mem_pool_t *pool, char *note_text)
{
    struct psx_parser_state_t _ps = {0};
    struct psx_parser_state_t *ps = &_ps;
    ps_init (ps, note_text);

    struct psx_block_t *root_block = psx_container_block_new(pool, BLOCK_TYPE_ROOT, 0);

    // Expect a title as the start of the note, fail if no title is found.
    struct psx_token_t tok = ps_next_peek (ps);
    if (tok.type != TOKEN_TYPE_TITLE) {
        ps->error = true;
        //ps->error_msg = sprintf("Notes must start with a Heading 1 title.");
    }


    // Parse note's content
    DYNAMIC_ARRAY_APPEND (ps->block_stack, root_block);
    while (!ps->is_eof && !ps->error) {
        int curr_block_idx = 0;
        struct psx_token_t tok = ps_next(ps);

        // Match the indentation of the received token.
        struct psx_block_t *next_stack_block = ps->block_stack[curr_block_idx+1];
        while (curr_block_idx+1 < ps->block_stack_len &&
               next_stack_block->type == BLOCK_TYPE_LIST &&
               (tok.margin > next_stack_block->margin ||
                   (tok.type == next_stack_block->list_type && tok.margin == next_stack_block->margin))) {
            curr_block_idx++;
            next_stack_block = ps->block_stack[curr_block_idx+1];
        }

        // Pop all blocks after the current index
        ps->block_stack_len = curr_block_idx+1;

        if (ps_match(ps, TOKEN_TYPE_TITLE, NULL)) {
            struct psx_block_t *heading_block = psx_push_block (ps, psx_leaf_block_new(pool, BLOCK_TYPE_HEADING, tok.margin, tok.value));
            heading_block->heading_number = tok.heading_number;

        } else if (ps_match(ps, TOKEN_TYPE_PARAGRAPH, NULL)) {
            struct psx_block_t *new_paragraph = psx_push_block (ps, psx_leaf_block_new(pool, BLOCK_TYPE_PARAGRAPH, tok.margin, tok.value));

            // Append all paragraph continuation lines. This ensures all paragraphs
            // found at the beginning of the iteration followed an empty line.
            struct psx_token_t tok_peek = ps_next_peek(ps);
            while (tok_peek.is_eol && tok_peek.type == TOKEN_TYPE_PARAGRAPH) {
                str_cat_c(&new_paragraph->inline_content, " ");
                strn_cat_c(&new_paragraph->inline_content, tok_peek.value.s, tok_peek.value.len);
                ps_next(ps);

                tok_peek = ps_next_peek(ps);
            }

        } else if (ps_match(ps, TOKEN_TYPE_CODE_HEADER, NULL)) {
            struct psx_block_t *new_code_block = psx_push_block (ps, psx_leaf_block_new(pool, BLOCK_TYPE_CODE, tok.margin, SSTRING("",0)));

            char *start = ps->pos;
            int min_leading_spaces = INT32_MAX;
            struct psx_token_t tok_peek = ps_next_peek(ps);
            while (tok_peek.is_eol && tok_peek.type == TOKEN_TYPE_CODE_LINE) {
                ps_next(ps);

                if (!is_empty_line(tok_peek.value)) {
                    int space_count = -1;
                    while (is_space (tok_peek.value.s + space_count + 1)) {
                        space_count++;
                    }

                    if (space_count >= 0) { // Ignore empty where we couldn't find any non-whitespace character.
                        min_leading_spaces = MIN (min_leading_spaces, space_count + 1);
                    }
                }

                tok_peek = ps_next_peek(ps);
            }
            ps_next(ps);

            // Concatenate lines to inline content while removing  the most
            // leading spaces we can remove. I call this automatic space
            // normalization.
            ps->pos = start;
            bool is_start = true; // Used to strip trailing empty lines.
            tok_peek = ps_next_peek(ps);
            while (tok_peek.is_eol && tok_peek.type == TOKEN_TYPE_CODE_LINE) {
                ps_next(ps);

                if (!is_start || !is_empty_line(tok_peek.value)) {
                    is_start = false;
                    if (min_leading_spaces < tok_peek.value.len) {
                        strn_cat_c(&new_code_block->inline_content, tok_peek.value.s+min_leading_spaces, tok_peek.value.len-min_leading_spaces);
                    } else {
                        str_cat_c(&new_code_block->inline_content, "\n");
                    }
                }

                tok_peek = ps_next_peek(ps);
            }

        } else if (ps_match(ps, TOKEN_TYPE_BULLET_LIST, NULL) || ps_match(ps, TOKEN_TYPE_NUMBERED_LIST, NULL)) {
            struct psx_block_t *prnt = ps->block_stack[curr_block_idx];
            if (prnt->type != BLOCK_TYPE_LIST ||
                tok.margin >= prnt->margin + prnt->content_start) {
                struct psx_block_t *list_block = psx_push_block (ps, psx_container_block_new(pool, BLOCK_TYPE_LIST, tok.margin));
                list_block->content_start = tok.content_start;
                list_block->list_type = tok.type;
            }

            psx_push_block(ps, psx_container_block_new(pool, BLOCK_TYPE_LIST_ITEM, tok.margin));

            struct psx_token_t tok_peek = ps_next_peek(ps);
            if (tok_peek.is_eol && tok_peek.type == TOKEN_TYPE_PARAGRAPH) {
                ps_next(ps);

                // Use list's margin... maybe this will never be read?...
                struct psx_block_t *new_paragraph = psx_push_block(ps, psx_leaf_block_new(pool, BLOCK_TYPE_PARAGRAPH, tok.margin, tok_peek.value));

                // Append all paragraph continuation lines. This ensures all paragraphs
                // found at the beginning of the iteration followed an empty line.
                tok_peek = ps_next_peek(ps);
                while (tok_peek.is_eol && tok_peek.type == TOKEN_TYPE_PARAGRAPH) {
                    strn_cat_c(&new_paragraph->inline_content, tok_peek.value.s, tok_peek.value.len);
                    ps_next(ps);

                    tok_peek = ps_next_peek(ps);
                }
            }
        }
    }

    //block_tree_user_callbacks (root_block)
    ps_destroy (ps);

    return root_block;
}

void str_cat_indented_debug_multiline (string_t *str, int curr_indent, char *c_str)
{
    char *pos = c_str;
    while (*pos != '\0') {
        char *start = pos;
        while (*pos != '\n' && *pos != '\0') pos++;

        str_cat_indented_printf (str, curr_indent, ECMA_S_YELLOW(0,"%.*s"), (int) (pos - start), start);
        if (*pos == '\n') {
            str_cat_printf (str, "↲");
            pos++;
        }

        if (*pos == '\0') {
            str_cat_printf (str, "∎\n");
        } else {
            str_cat_printf (str, "\n");
        }
    }

    //str_cat_printf (str, "'%s'\n", c_str);
}

void _str_cat_block_tree (string_t *str, struct psx_block_t *block, int indent, int curr_indent)
{
    str_cat_indented_printf (str, curr_indent, "type: %s\n", psx_block_type_names[block->type]);
    str_cat_indented_printf (str, curr_indent, "margin: %d\n", block->margin);
    str_cat_indented_printf (str, curr_indent, "heading_number: %d\n", block->heading_number);
    str_cat_indented_printf (str, curr_indent, "list_type: %s\n", psx_token_type_names[block->list_type]);
    str_cat_indented_printf (str, curr_indent, "content_start: %d\n", block->content_start);

    if (block->block_content == NULL) {
        str_cat_indented_printf (str, curr_indent, "inline_content:\n");
        str_cat_indented_debug_multiline (str, curr_indent, str_data(&block->inline_content));
        str_cat_printf (str, "\n");

    } else {
        str_cat_indented_printf (str, curr_indent, "block_content:\n");
        LINKED_LIST_FOR (struct psx_block_t*, curr_child, block->block_content) {
            _str_cat_block_tree (str, curr_child, indent, curr_indent + indent);
        }
    }
}

void str_cat_block_tree (string_t *str, struct psx_block_t *root, int indent)
{
    _str_cat_block_tree (str, root, indent, 0);
}

void printf_block_tree (struct psx_block_t *root, int indent)
{
    string_t str = {0};
    str_cat_block_tree (&str, root, indent);
    printf ("%s", str_data(&str));
    str_free (&str);
}

struct html_t* markup_to_html (mem_pool_t *pool, char *markup, char *id, int x)
{
    mem_pool_t pool_l = {0};
    struct html_t *html = mem_pool_push_struct (pool, struct html_t);
    *html = ZERO_INIT (struct html_t);
    html->pool = pool;

    string_t buff = {0};

    struct html_element_t *root = html_new_element (html, "div");
    html->root = root;
    html_element_attribute_set (html, root, "id", id);
    html_element_class_add (html, root, "note");
    html_element_class_add (html, root, "expanded");

    // TODO: Make html_element_attribute_set() receive printf parameters
    // and format string.
    str_set_printf (&buff, "%ipx", x);
    html_element_attribute_set (html, root, "style", str_data(&buff));

    struct psx_block_t *root_block = parse_note_text(&pool_l, markup);
    //printf_block_tree (root_block, 4);

    block_tree_to_html(html, root_block, root);

    mem_pool_destroy (&pool_l);

    return html;
}

#endif
