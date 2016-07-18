-- Copyright (c) 2016 Martin Ridgers
-- License: http://opensource.org/licenses/MIT

--------------------------------------------------------------------------------
local cmd_commands = {
    "assoc", "break", "call", "cd", "chcp", "chdir", "cls", "color", "copy",
    "date", "del", "dir", "diskcomp", "diskcopy", "echo", "endlocal", "erase",
    "exit", "for", "format", "ftype", "goto", "graftabl", "if", "md", "mkdir",
    "mklink", "more", "move", "path", "pause", "popd", "prompt", "pushd", "rd",
    "rem", "ren", "rename", "rmdir", "set", "setlocal", "shift", "start",
    "time", "title", "tree", "type", "ver", "verify", "vol"
}

--------------------------------------------------------------------------------
local function cmd_command_generator(line_state, match_builder)
    if line_state:getwordcount() > 1 then
        return false
    end

    if settings.get("exec.space_prefix") then
        local word_info = line_state:getwordinfo(1)
        local offset = 1
        if word_info.quoted then offset = 2 end
        if word_info.offset > offset then
            return false
        end
    end

    match_builder:add(cmd_commands)
    return false
end

--------------------------------------------------------------------------------
clink.register_match_generator(cmd_command_generator, 40)
