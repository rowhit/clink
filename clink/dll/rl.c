/* Copyright (c) 2012 Martin Ridgers
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pch.h"
#include "shared/util.h"

//------------------------------------------------------------------------------
void                initialise_lua();
char**              lua_generate_matches(const char*, int, int);
void                lua_filter_prompt(char*, int);
char**              lua_match_display_filter(char**, int);
void                initialise_rl_scroller();
void                enter_scroll_mode(int);
void                move_cursor(int, int);
void                initialise_clink_settings();
int                 get_clink_setting_int(const char*);
const char*         find_next_ansi_code(const char*, int*);

int                 g_slash_translation             = 0;
extern int          rl_visible_stats;
extern int          rl_display_fixed;
extern int          rl_editing_mode;
extern const char*  rl_filename_quote_characters;
extern int          _rl_complete_mark_directories;
extern char*        _rl_comment_begin;
static int          g_new_history_count             = 0;

//------------------------------------------------------------------------------
// This ensures the cursor is visible as printing to the console usually makes
// the cursor disappear momentarily.
static void display()
{
    rl_redisplay();
    move_cursor(0, 0);
}

//------------------------------------------------------------------------------
static void simulate_sigwinch(COORD expected_cursor_pos)
{
    // In the land of POSIX a terminal would raise a SIGWINCH signal when it is
    // resized. See rl_sigwinch_handler() in readline/signal.c.

    extern int _rl_vis_botlin;
    extern int _rl_last_v_pos;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    rl_voidfunc_t* redisplay_func_cache;
    int base_y;
    int bottom_line;
    HANDLE handle;

    bottom_line = _rl_vis_botlin - 1;
    handle = GetStdHandle(STD_OUTPUT_HANDLE);

    // Cache redisplay function. Need original as it handles redraw correctly.
    redisplay_func_cache = rl_redisplay_function;
    rl_redisplay_function = rl_redisplay;

    // Cursor may be out of sync with where Readline expects the cursor to be.
    // Put it back where it was, clamping if necessary.
    GetConsoleScreenBufferInfo(handle, &csbi);
    if (expected_cursor_pos.X >= csbi.dwSize.X)
    {
        expected_cursor_pos.X = csbi.dwSize.X - 1;
    }
    if (expected_cursor_pos.Y >= csbi.dwSize.Y)
    {
        expected_cursor_pos.Y = csbi.dwSize.Y - 1;
    }
    SetConsoleCursorPosition(handle, expected_cursor_pos);

    // Let Readline handle the buffer resize.
    RL_SETSTATE(RL_STATE_SIGHANDLER);
    rl_resize_terminal();
    RL_UNSETSTATE(RL_STATE_SIGHANDLER);

    rl_redisplay_function = redisplay_func_cache;

    // Now some redraw edge cases need to be handled.
    GetConsoleScreenBufferInfo(handle, &csbi);
    base_y = csbi.dwCursorPosition.Y - _rl_last_v_pos;

    if (bottom_line > _rl_vis_botlin)
    {
        // Readline SIGWINCH handling assumes that at most one line needs to
        // be cleared which is not the case when resizing from small to large
        // widths.

        CHAR_INFO fill;
        SMALL_RECT rect;
        COORD coord;

        rect.Left = 0;
        rect.Right = csbi.dwSize.X;
        rect.Top = base_y + _rl_vis_botlin + 1;
        rect.Bottom = base_y + bottom_line;

        fill.Char.AsciiChar = ' ';
        fill.Attributes = csbi.wAttributes;

        coord.X = rect.Right + 1;
        coord.Y = rect.Top;

        ScrollConsoleScreenBuffer(handle, &rect, NULL, coord, &fill);
    }
    else
    {
        // Readline never writes to the last column as it wraps the cursor. The
        // last column will have noise when making the width smaller. Clear it.

        CHAR_INFO fill;
        SMALL_RECT rect;
        COORD coord;

        rect.Left = rect.Right = csbi.dwSize.X - 1;
        rect.Top = base_y;
        rect.Bottom = base_y + _rl_vis_botlin;

        fill.Char.AsciiChar = ' ';
        fill.Attributes = csbi.wAttributes;

        coord.X = rect.Right + 1;
        coord.Y = rect.Top;

        ScrollConsoleScreenBuffer(handle, &rect, NULL, coord, &fill);
    }
}

//------------------------------------------------------------------------------
//#define DEBUG_GETC
/*
    Taken from msvcrt.dll's getextendedkeycode()

                          ELSE SHFT CTRL ALTS
    00000000`723d36e0  1c 000d 000d 000a a600
    00000000`723d36ea  35 002f 003f 9500 a400
    00000000`723d36f4  47 47e0 47e0 77e0 9700
    00000000`723d36fe  48 48e0 48e0 8de0 9800
    00000000`723d3708  49 49e0 49e0 86e0 9900
    00000000`723d3712  4b 4be0 4be0 73e0 9b00
    00000000`723d371c  4d 4de0 4de0 74e0 9d00
    00000000`723d3726  4f 4fe0 4fe0 75e0 9f00
    00000000`723d3730  50 50e0 50e0 91e0 a000
    00000000`723d373a  51 51e0 51e0 76e0 a100
    00000000`723d3744  52 52e0 52e0 92e0 a200
    00000000`723d374e  53 53e0 53e0 93e0 a300
*/
static int getc_internal(int* alt)
{
    static int       carry        = 0; // Multithreading? What's that?
    static const int CTRL_PRESSED = LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED;

    int key_char;
    int key_vk;
    int key_sc;
    int key_flags;
    HANDLE handle;
    DWORD mode;

    // Clear all flags so the console doesn't do anything special. This prevents
    // key presses such as Ctrl-C and Ctrl-S from being swallowed.
    handle = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(handle, &mode);

loop:
    key_char = 0;
    key_vk = 0;
    key_sc = 0;
    key_flags = 0;
    *alt = 0;

    // Read a key or use what was carried across from a previous call.
    if (carry)
    {
        key_char = carry;
        carry = 0;
    }
    else
    {
        HANDLE handle_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        DWORD count;
        INPUT_RECORD record;
        const KEY_EVENT_RECORD* key;

        GetConsoleScreenBufferInfo(handle_stdout, &csbi);

        // Fresh read from the console.
        SetConsoleMode(handle, ENABLE_WINDOW_INPUT);
        ReadConsoleInputW(handle, &record, 1, &count);

        // Simulate SIGWINCH signals.
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
        {
            simulate_sigwinch(csbi.dwCursorPosition);
            goto loop;
        }

        if (record.EventType != KEY_EVENT)
        {
            goto loop;
        }

        key = &record.Event.KeyEvent;
        if (key->bKeyDown == FALSE)
        {
            goto loop;
        }

        key_char = key->uChar.UnicodeChar;
        key_vk = key->wVirtualKeyCode;
        key_sc = key->wVirtualScanCode;
        key_flags = key->dwControlKeyState;

        *alt = !!(key_flags & LEFT_ALT_PRESSED);

#if defined(DEBUG_GETC) && defined(_DEBUG)
        {
            int i;
            puts("");
            for (i = 0; i < sizeof(*key); ++i)
            {
                printf("%02x ", ((unsigned char*)key)[i]);
            }
        }
#endif
    }

    // No Unicode character? Then some post-processing is required to make the
    // output compatible with whatever standard Linux terminals adhere to and
    // that which Readline expects.
    if (key_char == 0)
    {
        // Differentiate enhanced keys depending on modifier key state. MSVC's
        // runtime does something similar. Slightly non-standard.
        if (key_flags & ENHANCED_KEY)
        {
            int i;
            static const int mod_map[][4] =
            {
                //Nrml  Shft  Ctrl  CtSh
                { 0x47, 0x61, 0x77, 0x21 }, // home
                { 0x48, 0x62, 0x54, 0x22 }, // up
                { 0x49, 0x63, 0x55, 0x23 }, // pgup
                { 0x4b, 0x64, 0x73, 0x24 }, // left
                { 0x4d, 0x65, 0x74, 0x25 }, // right
                { 0x4f, 0x66, 0x75, 0x26 }, // end
                { 0x50, 0x67, 0x56, 0x27 }, // down
                { 0x51, 0x68, 0x76, 0x28 }, // pgdn
                { 0x52, 0x69, 0x57, 0x29 }, // insert
                { 0x53, 0x6a, 0x58, 0x2a }, // delete
            };

            for (i = 0; i < sizeof_array(mod_map); ++i)
            {
                int j = 0;
                if (mod_map[i][j] != key_sc)
                {
                    continue;
                }

                j += !!(key_flags & SHIFT_PRESSED);
                j += !!(key_flags & CTRL_PRESSED) << 1;
                carry = mod_map[i][j];
                break;
            }

            // Blacklist.
            if (!carry)
            {
                goto loop;
            }

            key_vk = 0xe0;
        }
        // This builds Ctrl-<key> map to match that as described by Readline's
        // source for the emacs/vi keymaps.
        #define CONTAINS(l, r) (unsigned)(key_vk - l) <= (r - l)
        else if (CONTAINS('A', 'Z'))    key_vk -= 'A' - 1;
        else if (CONTAINS(0xdb, 0xdd))  key_vk -= 0xdb - 0x1b;
        else if (key_vk == 0x32)        key_vk = 0;
        else if (key_vk == 0x36)        key_vk = 0x1e;
        else if (key_vk == 0xbd)        key_vk = 0x1f;
        else                            goto loop;
        #undef CONTAINS

        key_char = key_vk;
    }

#if defined(DEBUG_GETC) && defined(_DEBUG)
    printf("\n%08x '%c'", key_char, key_char);
#endif

    SetConsoleMode(handle, mode);
    return key_char;
}

//------------------------------------------------------------------------------
static int getc_impl(FILE* stream)
{
    int alt;
    int i;
    while (1)
    {
        wchar_t wc[2];
        char utf8[4];

        alt = 0;
        i = GETWCH_IMPL(&alt);

        // Treat esc like cmd.exe does - clear the line.
        if (i == 0x1b)
        {
            if (rl_editing_mode == emacs_mode &&
                get_clink_setting_int("esc_clears_line"))
            {
                using_history();
                rl_delete_text(0, rl_end);
                rl_point = 0;
                display();
                continue;
            }
        }

        // Mask off top bits, they're used to track ALT key state.
        if (i < 0x80 || i == 0xe0)
        {
            break;
        }

        // Convert to utf-8 and insert directly into rl's line buffer.
        wc[0] = (wchar_t)i;
        wc[1] = L'\0';

        WideCharToMultiByte(
            CP_UTF8, 0,
            wc, -1,
            utf8, sizeof(utf8),
            NULL,
            NULL
        );

        rl_insert_text(utf8);
        display();
    }

    alt = RL_ISSTATE(RL_STATE_MOREINPUT) ? 0 : alt;
    alt = alt ? 0x80 : 0;
    return i|alt;
}

//------------------------------------------------------------------------------
static void translate_matches(char** matches, char from, char to)
{
    char** m;

    if (!rl_filename_completion_desired)
    {
        return;
    }

    // Convert forward slashes to back slashes
    m = matches;
    while (*m)
    {
        char* c = *m;
        while (*c)
        {
            *c = (*c == from) ? to : *c;
            ++c;
        }

        ++m;
    }
}

//------------------------------------------------------------------------------
static void quote_matches(char** matches)
{
    // The least disruptive way to inject quotes into the command line is do it
    // at the last possible moment. Either the first match (the lcd) needs a
    // quote, or the next character the user may type needs quoting

    char** m;
    int need_quote;
    int lcd_length;
    int rl_will_quote;

    // Does the lcd have a quote char? Readline will add the quote if it thinks
    // it's completing file names.
    rl_will_quote = strpbrk(matches[0], rl_filename_quote_characters) != NULL;
    rl_will_quote &= (rl_filename_completion_desired != 0);
    if (rl_will_quote)
    {
        return;
    }

    if (rl_completion_quote_character == '\"')
    {
        return;
    }

    // Check other matches for characters that need quoting.
    need_quote = 0;
    lcd_length = (int)strlen(matches[0]);
    m = matches + 1;
    while (*m && !need_quote)
    {
        int i;

        i = strlen(*m);
        if (i > lcd_length)
        {
            int c = *(*m + lcd_length);
            need_quote = strchr(rl_filename_quote_characters, c) != NULL;
        }

        ++m;
    }

    // So... do we need to prepend a quote?
    if (need_quote)
    {
        char* c = malloc(strlen(matches[0]) + 2);
        strcpy(c + 1, matches[0]);
        free(matches[0]);

        c[0] = '\"';
        matches[0] = c;
    }
}

//------------------------------------------------------------------------------
static int postprocess_matches(char** matches)
{
    char** m;
    int need_quote;
    int first_needs_quotes;

    if (g_slash_translation >= 0)
    {
        switch (g_slash_translation)
        {
        case 1:     translate_matches(matches, '\\', '/');  break;
        default:    translate_matches(matches, '/', '\\');  break;
        }
    }

    quote_matches(matches);

    return 0;
}

//------------------------------------------------------------------------------
static void suffix_translation()
{
    // readline's path completion may have appended a '/'. If so; flip it.

    char from;
    char* to;

    if (g_slash_translation < 0)
    {
        return;
    }

    // Decide what direction we're going in.
    switch (g_slash_translation)
    {
    case 1:     from = '\\'; to = "/";  break;
    default:    from = '/';  to = "\\"; break;
    }

    // Swap the training slash, using readline's API to maintain undo state.
    if ((rl_point > 0) && (rl_line_buffer[rl_point - 1] == from))
    {
        rl_delete_text(rl_point - 1, rl_point);
        --rl_point;
        rl_insert_text(to);
    }
}

//------------------------------------------------------------------------------
static int completion_shim(int count, int invoking_key)
{
    int ret;

    // rl_complete checks if it was called previously.
    if (rl_last_func == completion_shim)
    {
        rl_last_func = rl_complete;
    }

    rl_begin_undo_group();
    ret = rl_complete(count, invoking_key);
    suffix_translation();
    rl_end_undo_group();

    g_slash_translation = 0;

    return ret;
}

//------------------------------------------------------------------------------
static char** alternative_matches(const char* text, int start, int end)
{
    char* c;
    char** lua_matches;

    // Try the lua match generators first
    lua_matches = lua_generate_matches(text, start, end);
    if (lua_matches != NULL)
    {
        rl_attempted_completion_over = 1;
        return (lua_matches[0] != NULL) ? lua_matches : NULL;
    }

    // We're going to use readline's path completion, which only works with
    // forward slashes. So, we slightly modify the completion state here.
    c = (char*)text;
    while (*c)
    {
        *c = (*c == '\\') ? '/' : *c;
        ++c;
    }

    return NULL;
}

//------------------------------------------------------------------------------
char** match_display_filter(char** matches, int match_count)
{
    int i;
    char** new_matches;

    ++match_count;

    // First, see if there's a Lua function registered to filter matches for
    // display (this is set via clink.match_display_filter).
    new_matches = lua_match_display_filter(matches, match_count);
    if (new_matches != NULL)
    {
        return new_matches;
    }

    // The matches need to be processed so needless path information is removed
    // (this is caused by the \ and / hurdles).
    new_matches = (char**)calloc(1, match_count * sizeof(char*));
    for (i = 0; i < match_count; ++i)
    {
        int is_dir = 0;
        int len;
        char* base = NULL;
        
        // If matches are files then strip off the path and establish if they
        // are directories.
        if (rl_filename_completion_desired)
        {
            DWORD file_attrib;

            base = strrchr(matches[i], '\\');
            if (base == NULL)
            {
                base = strrchr(matches[i], ':');
            }

            // Is this a dir?
            file_attrib = GetFileAttributes(matches[i]);
            if (file_attrib != INVALID_FILE_ATTRIBUTES)
            {
                is_dir = !!(file_attrib & FILE_ATTRIBUTE_DIRECTORY);
            }
        }
        base = (base == NULL) ? matches[i] : base + 1;
        len = (int)strlen(base) + is_dir;

        new_matches[i] = malloc(len + 1);
        if (is_dir)
        {
            // Coming soon; colours!
            strcpy(new_matches[i], base);
            strcat(new_matches[i], "\\");
        }
        else
        {
            strcpy(new_matches[i], base);
        }
    }

    return new_matches;
}

//------------------------------------------------------------------------------
static void display_matches(char** matches, int match_count, int longest)
{
    int i;
    char** new_matches;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    WORD text_attrib;
    HANDLE std_out_handle;
    wchar_t buffer[512];
    int show_matches = 2;
    int match_colour;

    // Process matches and recalculate the longest match length.
    new_matches = match_display_filter(matches, match_count);

    longest = 0;
    for (i = 0; i < (match_count + 1); ++i)
    {
        int len = (int)strlen(new_matches[i]);
        longest = (len > longest) ? len : longest;
    }

    std_out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(std_out_handle, &csbi);

    // Get the console's current colour settings
    match_colour = get_clink_setting_int("match_colour");
    if (match_colour == -1)
    {
        // Pick a suitable foreground colour, check fg isn't the same as bg, and set.
        text_attrib = csbi.wAttributes;
        text_attrib ^= 0x08;

        if ((text_attrib & 0xf0) == (text_attrib & 0x0f))
        {
            text_attrib ^= FOREGROUND_INTENSITY;
        }
    }
    else
    {
        text_attrib = csbi.wAttributes & 0xf0;
        text_attrib |= (match_colour & 0x0f);
    }

    SetConsoleTextAttribute(std_out_handle, text_attrib);

    // If there's lots of matches, check with the user before displaying them
    // This matches readline's behaviour, which will get skipped (annoyingly)
    if ((rl_completion_query_items > 0) &&
        (match_count >= rl_completion_query_items))
    {
        DWORD written;

        _snwprintf(
            buffer,
            sizeof_array(buffer),
            L"\nDisplay all %d possibilities? (y or n)",
            match_count
        );
        WriteConsoleW(std_out_handle, buffer, wcslen(buffer), &written, NULL);

        while (show_matches > 1)
        {
            int c = rl_read_key();
            switch (c)
            {
            case 'y':
            case 'Y':
            case ' ':
                show_matches = 1;
                break;

            case 'n':
            case 'N':
            case 0x7f:
                show_matches = 0;
                break;
            }
        }
    }

    // Get readline to display the matches.
    if (show_matches > 0)
    {
        // Turn of '/' suffix for directories. RL assumes '/', which isn't the
        // case, plus clink uses colours instead.
        int j = _rl_complete_mark_directories;
        _rl_complete_mark_directories = 0;

        rl_display_match_list(new_matches, match_count, longest);

        _rl_complete_mark_directories = j;
    }
    else
    {
        rl_crlf();
    }

    // Reset console colour back to normal.
    SetConsoleTextAttribute(std_out_handle, csbi.wAttributes);
    rl_forced_update_display();
    rl_display_fixed = 1;

    // Tidy up.
    for (i = 0; i < match_count; ++i)
    {
        free(new_matches[i]);
    }
    free(new_matches);
}

//------------------------------------------------------------------------------
static void get_history_file_name(char* buffer, int size)
{
    get_config_dir(buffer, size);
    if (buffer[0])
    {
        str_cat(buffer, "/.history", size);
    }
}

//------------------------------------------------------------------------------
static void load_history()
{
    char buffer[1024];
    get_history_file_name(buffer, sizeof(buffer));
    read_history(buffer);
    using_history();
}

//------------------------------------------------------------------------------
void save_history()
{
    int max_history;
    char buffer[1024];
    const char* c;

    get_history_file_name(buffer, sizeof(buffer));

    // Get max history size.
    c = rl_variable_value("history-size");
    max_history = (c != NULL) ? atoi(c) : 1000;

    // Write new history to the file, and truncate to our maximum.
    if (append_history(g_new_history_count, buffer) != 0)
    {
        write_history(buffer);
    }

    history_truncate_file(buffer, max_history);
}

//------------------------------------------------------------------------------
static void clear_line()
{
    using_history();
    rl_delete_text(0, rl_end);
    rl_point = 0;
}

//------------------------------------------------------------------------------
static int ctrl_c(int count, int invoking_key)
{
    if (get_clink_setting_int("passthrough_ctrlc"))
    {
        rl_line_buffer[0] = '\x03';
        rl_line_buffer[1] = '\x00';
        rl_point = 1;
        rl_end = 1;
        rl_done = 1;
    }
    else
    {
        clear_line();
    }

    return 0;
}

//------------------------------------------------------------------------------
static int paste_from_clipboard(int count, int invoking_key)
{
    if (OpenClipboard(NULL) != FALSE)
    {
        HANDLE clip_data = GetClipboardData(CF_UNICODETEXT);
        if (clip_data != NULL)
        {
            wchar_t* from_clipboard = (wchar_t*)clip_data;
            char utf8[1024];

            WideCharToMultiByte(
                CP_UTF8, 0,
                from_clipboard, -1,
                utf8, sizeof(utf8),
                NULL, NULL
            );
            utf8[sizeof(utf8) - 1] = '\0';

            rl_insert_text(utf8);
        }

        CloseClipboard();
    }
    
    return 0;
}

//------------------------------------------------------------------------------
static int up_directory(int count, int invoking_key)
{
    rl_begin_undo_group();
    rl_delete_text(0, rl_end);
    rl_point = 0;
    rl_insert_text("cd ..");
    rl_end_undo_group();
    rl_done = 1;

    return 0;
}

//------------------------------------------------------------------------------
static int page_up(int count, int invoking_key)
{
    enter_scroll_mode(-1);
    return 0;
}

//------------------------------------------------------------------------------
static int filter_prompt()
{
    char* next;
    char prompt[1024];
    char tagged_prompt[sizeof_array(prompt)];

    // Get the prompt from Readline and pass it to Clink's filter framework
    // in Lua.
    prompt[0] = '\0';
    str_cat(prompt, rl_prompt, sizeof_array(prompt));

    lua_filter_prompt(prompt, sizeof_array(prompt));

    // Scan for ansi codes and surround them with Readline's markers for
    // invisible characters.
    tagged_prompt[0] ='\0';
    next = prompt;
    while (*next)
    {
        static const int tp_size = sizeof_array(tagged_prompt);

        int size;
        char* code;

        code = (char*)find_next_ansi_code(next, &size);
        str_cat_n(tagged_prompt, next, tp_size, code - next);
        if (*code)
        {
            static const char* tags[] = { "\001", "\002" };

            str_cat(tagged_prompt, tags[0], tp_size);
            str_cat_n(tagged_prompt, code, tp_size, size);
            str_cat(tagged_prompt, tags[1], tp_size);
        }

        next = code + size;
    }

    rl_set_prompt(tagged_prompt);
    return 0;
}

//------------------------------------------------------------------------------
static int initialise_hook()
{
    rl_redisplay_function = display;
    rl_getc_function = getc_impl;

    // Invalid filename characters; <>|?*:"\/
    _rl_comment_begin = "::";
    rl_completer_quote_characters = "\"";
    rl_ignore_some_completions_function = postprocess_matches;
    rl_basic_word_break_characters = " <>|=;";
    rl_completer_word_break_characters = (char*)rl_basic_word_break_characters;
    rl_attempted_completion_function = alternative_matches;
    if (rl_completion_display_matches_hook == NULL)
    {
        rl_completion_display_matches_hook = display_matches;
    }

    rl_basic_quote_characters = "\"";
    rl_filename_quote_characters = " %=;&^";

    rl_add_funmap_entry("clink-completion-shim", completion_shim);
    rl_add_funmap_entry("ctrl-c", ctrl_c);
    rl_add_funmap_entry("paste-from-clipboard", paste_from_clipboard);
    rl_add_funmap_entry("page-up", page_up);
    rl_add_funmap_entry("up-directory", up_directory);

    initialise_clink_settings();
    initialise_lua();
    initialise_rl_scroller();
    load_history();
    rl_re_read_init_file(0, 0);
    rl_visible_stats = 0;               // serves no purpose under win32.

    rl_startup_hook = filter_prompt;
    return filter_prompt();
}

//------------------------------------------------------------------------------
static void add_to_history(const char* line)
{
    const unsigned char* c;
    HIST_ENTRY* hist_entry;

    // Skip leading whitespace
    c = (const unsigned char*)line;
    while (*c)
    {
        if (!isspace(*c) && (*c != '\x03'))
        {
            break;
        }

        ++c;
    }

    // Skip empty lines
    if (*c == '\0')
    {
        return;
    }

    // Check the line's not a duplicate of the last in the history.
    using_history();
    hist_entry = previous_history();
    if (hist_entry != NULL)
    {
        if (strcmp(hist_entry->line, c) == 0)
        {
            return;
        }
    }

    // All's well. Add the line.
    using_history();
    add_history(line);

    ++g_new_history_count;
}

//------------------------------------------------------------------------------
int call_readline(
    const wchar_t* prompt,
    wchar_t* result,
    unsigned size
)
{
    static int initialised = 0;
    unsigned text_size;
    int expand_result;
    char* text;
    char* expanded;
    char prompt_utf8[1024];
    char cwd_cache[MAX_PATH];

    // Convery prompt to utf-8.
    WideCharToMultiByte(
        CP_UTF8, 0,
        prompt, -1,
        prompt_utf8, sizeof(prompt_utf8),
        NULL,
        NULL
    );

    // Initialisation (then prompt filtering after that)
    if (!initialised)
    {
        rl_startup_hook = initialise_hook;
        initialised = 1;
    }

    GetCurrentDirectory(sizeof_array(cwd_cache), cwd_cache);

    // Call readline
    do
    {
        text = readline(prompt_utf8);
        if (!text)
        {
            // EOF situation.
            return 1;
        }

        // Expand history designators in returned buffer.
        expanded = NULL;
        expand_result = history_expand(text, &expanded);
        if (expand_result < 0)
        {
            free(expanded);
        }
        else
        {
            free(text);
            text = expanded;

            // If there was some expansion then display the expanded result.
            if (expand_result > 0)
            {
                hooked_fprintf(NULL, "History expansion: %s\n", text);
            }
        }

        add_to_history(text);
    }
    while (!text || expand_result == 2);

    SetCurrentDirectory(cwd_cache);

    text_size = MultiByteToWideChar(CP_UTF8, 0, text, -1, result, 0);
    text_size = (size < text_size) ? size : strlen(text);
    text_size = MultiByteToWideChar(CP_UTF8, 0, text, -1, result, size);
    result[size - 1] = L'\0';

    free(text);
    return 0;
}

// vim: expandtab