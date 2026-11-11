/* Large MI replies must be parsed and freed without stack usage proportional
 * to the number of files. Run via make check; no debugger is required. */
#include "gdbwire.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

static const unsigned file_count = 100000;

static void check(bool condition)
{
    if (!condition) {
        std::fputs("FAIL: large source-list reply\n", stderr);
        std::abort();
    }
}

static void output(void *context, gdbwire_mi_output *out)
{
    gdbwire_mi_command *command = nullptr;
    check(out->kind == GDBWIRE_MI_OUTPUT_RESULT);
    check(gdbwire_get_mi_command(GDBWIRE_MI_FILE_LIST_EXEC_SOURCE_FILES,
        out->variant.result_record, &command) == GDBWIRE_OK);
    unsigned count = 0;
    for (auto *file = command->variant.file_list_exec_source_files.files;
            file; file = file->next, ++count) {
        std::string name = "file" + std::to_string(count) + ".cpp";
        check(file->file && name == file->file);
        check(file->fullname && "/src/" + name == file->fullname);
    }
    check(count == file_count);
    gdbwire_mi_command_free(command);
    gdbwire_mi_output_free(out);
    ++*static_cast<unsigned *>(context);
}

int main()
{
    std::string reply = "1^done,files=[";
    for (unsigned i = 0; i < file_count; ++i) {
        std::string name = "file" + std::to_string(i) + ".cpp";
        if (i)
            reply += ',';
        reply += "{file=\"" + name + "\",fullname=\"/src/" + name + "\"}";
    }
    reply += "]\n";
    unsigned records = 0;
    gdbwire_mi_parser_callbacks callbacks = {};
    callbacks.context = &records;
    callbacks.gdbwire_mi_output_callback = output;
    auto *parser = gdbwire_mi_parser_create(callbacks);
    check(parser != nullptr);
    for (size_t i = 0; i < reply.size(); i += 4096)
        check(gdbwire_mi_parser_push_data(parser, reply.data() + i,
            std::min(size_t(4096), reply.size() - i)) == GDBWIRE_OK);
    check(records == 1);
    gdbwire_mi_parser_destroy(parser);
    std::puts("PASS: parse, validate and free 100000 source files");
}
