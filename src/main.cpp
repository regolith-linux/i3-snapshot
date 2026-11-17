/*
    Copyright (c) 2019, Ken Gilmer
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    3. All advertising materials mentioning features or use of this software
       must display the following acknowledgement:
       This product includes software developed by Ken Gilmer.
    4. Neither the name of Ken Gilmer nor the
       names of its contributors may be used to endorse or promote products
       derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>
#include <i3ipc++/ipc.hpp>
#include <i3ipc++/ipc-util.hpp>
#include <json/json.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <zconf.h>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <vector>

#include "base64.h"

using namespace std;

/**
 * Keep track of output and workspace as the i3 container tree is traversed depth-first.
 */
struct TreeState {
    string outputName;
    string workspaceName;
    size_t workspaceId{};
    // Snapshot lines are collected first so nothing is printed when a window cannot be saved.
    ostringstream snapshot;
    bool unsaveable{};
};

/**
 * One window line of a snapshot, checked and decoded.
 */
struct SnapshotEntry {
    string outputName;
    string workspaceName;
    uint64_t workspaceId{};
    uint64_t windowId{};
    string windowTitle;
};

enum WindowIdentifier {
    I3_ID, WINDOW_TITLE
};

struct CommandLineOptions {
    bool debug;
    bool failFast;
    bool forceOutputMode;
    bool encodeStrings;
    bool dryRun;
    WindowIdentifier windowIdentifier;
};

/**
 * Determine if the i3 container is a window type.
 * @param c i3 container (a node of the GET_TREE reply)
 * @return true if container is window, false otherwise.
 */
bool isWindow(const Json::Value &c) {
    const Json::Value &window = c["window"];
    return c["type"].asString() == "con" && window.isIntegral() && window.asUInt64() != 0;
}

/**
 * Determine if container should not be ignored.
 * @param c i3 container
 * @return true if container is valid, false otherwise.
 */
bool isValidParent(const Json::Value &c) {
    return c["type"].asString() != "dockarea";
}

/**
 * Read i3's layout tree. i3ipc++'s get_tree() drops the floating_nodes of
 * each container, so the tree is read and parsed here.
 * @param sockfd socket of an open i3 IPC connection
 * @return the GET_TREE reply
 */
Json::Value getTree(int32_t sockfd) {
    auto buf = i3ipc::i3_msg(sockfd, i3ipc::ClientMessageType::GET_TREE);

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(string(buf->payload, buf->header->size), root, false)) {
        cerr << "Failed to parse the i3 tree: " << reader.getFormattedErrorMessages() << endl;
        exit(1);
    }
    return root;
}

/**
 * Read i3's layout tree over a connection of its own.
 * @return the GET_TREE reply
 */
Json::Value getTree() {
    int32_t sockfd = i3ipc::i3_connect(i3ipc::get_socketpath());
    Json::Value root = getTree(sockfd);
    i3ipc::i3_disconnect(sockfd);
    return root;
}

/**
 * Find a container by its i3 id.
 * @param c i3 container to search, with its descendants
 * @param id i3 container id
 * @return the container, or null if there is none with this id.
 */
const Json::Value *findContainer(const Json::Value &c, uint64_t id) {
    if (c["id"].isIntegral() && c["id"].asUInt64() == id) return &c;

    for (const char *key : {"nodes", "floating_nodes"}) {
        for (const auto &node : c[key]) {
            const Json::Value *found = findContainer(node, id);
            if (found) return found;
        }
    }
    return nullptr;
}

/**
 * Traverse i3 containers and emit relevant info to stdout.
 *
 * @param c i3 container
 * @param treeState storage of current state of tree traversal.
 */
void findWindows(const Json::Value &c, TreeState &treeState, CommandLineOptions &options) {
    const string type = c["type"].asString();
    const string name = c["name"].asString();

    if (type == "output") {
        treeState.outputName = name;
    } else if (type == "workspace") {
        treeState.workspaceName = name;
        treeState.workspaceId = c["id"].asUInt64();
    } else if (isWindow(c)) {
        if (treeState.outputName.empty() || treeState.workspaceName.empty()) {
            cout << "Invalid tree state, aborting." << endl;
            exit(1);
        }

        string outputEncoded;
        string workspaceEncoded;
        string windowEncoded;

        if (!options.encodeStrings) {
            // Plain snapshots separate fields with single spaces and lines with newlines, so these
            // values could not be read back as they were written.
            static const string unsafeInName(" \r\n\0", 4);
            static const string unsafeInTitle("\r\n\0", 3);

            if (treeState.outputName.find_first_of(unsafeInName) != string::npos
                || treeState.workspaceName.find_first_of(unsafeInName) != string::npos
                || name.find_first_of(unsafeInTitle) != string::npos) {
                cerr << "Cannot save window " << c["id"].asUInt64()
                     << " with raw strings: its output or workspace name contains a space, carriage return,"
                     << " line feed or NUL, or its title contains a carriage return, line feed or NUL."
                     << " Save without -r instead." << endl;
                treeState.unsaveable = true;
            }
        }

        if (options.encodeStrings) {
            outputEncoded = base64_encode(reinterpret_cast<const unsigned char *>(treeState.outputName.c_str()),
                                                 treeState.outputName.length());
            workspaceEncoded = base64_encode(
                    reinterpret_cast<const unsigned char *>(treeState.workspaceName.c_str()),
                    treeState.workspaceName.length());
            windowEncoded = base64_encode(
                    reinterpret_cast<const unsigned char *>(name.c_str()),
                    name.length());
        } else {
            outputEncoded = treeState.outputName;
            workspaceEncoded = treeState.workspaceName;
            windowEncoded = name;
        }

        // Output Name, Workspace Name, Workspace Id, Window Id, Window Name
        treeState.snapshot << outputEncoded << " " << workspaceEncoded << " " <<  treeState.workspaceId << " "
                           << c["id"].asUInt64() << " " << windowEncoded << endl;
    }

    if (isValidParent(c)) {
        // Tiled windows are in nodes, floating ones in floating_nodes
        for (const auto &node : c["nodes"])
            findWindows(node, treeState, options);
        for (const auto &node : c["floating_nodes"])
            findWindows(node, treeState, options);
    }
}

/**
 * Quote a value so that i3 reads it as exactly one argument. Inside double quotes i3 only
 * unescapes \" and \\, and nothing else ends the argument or starts another command.
 * @param value any string
 * @return the quoted argument
 */
string quoteArgument(const string &value) {
    stringstream quoted;
    quoted << std::quoted(value);
    return quoted.str();
}

/**
 * Build a criteria value matching exactly the given text. i3 criteria such as title and
 * workspace are regular expressions, so the text is anchored at both ends and quoted literally.
 * @param text the complete title or workspace name
 * @return a quoted criteria value
 */
string literalCriteria(const string &text) {
    string pattern = "\\A\\Q";
    size_t start = 0;
    size_t end;
    // \E would end the literal section early: end it, match a backslash and an E, start again.
    while ((end = text.find("\\E", start)) != string::npos) {
        pattern.append(text, start, end - start).append("\\E\\\\E\\Q");
        start = end + 2;
    }
    pattern.append(text, start, string::npos).append("\\E\\z");
    return quoteArgument(pattern);
}

/**
 * Move a workspace to an out and a window to a workspace.
 * @param i3conn i3 connection, null in dry run mode
 * @param windowId i3 window id
 * @param outputName system name for output (monitor)
 * @param workspaceName i3 name for workspace
 * @param workspaceId i3 id for workspace
 * @param windowTitle window title
 * @param opts command line options
 * @return true if operation success, false otherwise.
 */
bool
moveWindow(i3ipc::connection *i3conn, uint64_t windowId, const string &outputName, const string &workspaceName,
           uint64_t workspaceId, const string &windowTitle, CommandLineOptions &opts) {
    // An empty title names no window.
    if (opts.windowIdentifier == WINDOW_TITLE && windowTitle.empty()) return false;

    // https://build.i3wm.org/docs/userguide.html#command_criteria
    string windowCriteria;
    if (opts.windowIdentifier == I3_ID) {
        windowCriteria = "[con_id=" + to_string(windowId) + "]";
    } else {
        windowCriteria = "[title=" + literalCriteria(windowTitle) + "]";
    }

    // Move workspace to output
    // i3-msg [workspace="\\A\\Q2: web\\E\\z"] move workspace to output "eDP-1"
    string wsCmd;
    if (opts.windowIdentifier == I3_ID) {
        wsCmd = "[con_id=" + to_string(workspaceId) + "] move workspace to output " + quoteArgument(outputName);
    } else {
        wsCmd = "[workspace=" + literalCriteria(workspaceName) + "] move workspace to output "
                + quoteArgument(outputName);
    }

    // i3 ids are only meaningful while their container lives: once a workspace or window is gone,
    // its id may be given to a container created later. Ids are therefore only used while they
    // still name the workspace and the window of the snapshot.
    bool workspaceExists = true;
    if (!opts.dryRun && opts.windowIdentifier == I3_ID) {
        Json::Value tree = getTree(i3conn->get_main_socket_fd());
        const Json::Value *workspace = findContainer(tree, workspaceId);
        const Json::Value *window = findContainer(tree, windowId);

        workspaceExists = workspace && (*workspace)["type"].asString() == "workspace"
                          && (*workspace)["name"].asString() == workspaceName;
        if (!window || !isWindow(*window)) return false;
    }

    // This fails when the workspace no longer exists. Moving the window
    // below creates it again, and the workspace is then moved through the
    // window.
    bool workspaceMoved = false;
    if (workspaceExists) {
        if (opts.debug) cout << "i3-msg " << wsCmd << endl;

        workspaceMoved = opts.dryRun || i3conn->send_command(wsCmd);
    }

    // Move window to workspace
    string windowCmd = windowCriteria + " move container to workspace " + quoteArgument(workspaceName);

    if (opts.debug) cout << "i3-msg " << windowCmd << endl;

    if (opts.dryRun) return true;

    if (!i3conn->send_command(windowCmd)) return false;
    if (workspaceMoved) return true;

    string recreatedWsCmd = windowCriteria + " move workspace to output " + quoteArgument(outputName);

    if (opts.debug) cout << "i3-msg " << recreatedWsCmd << endl;

    return i3conn->send_command(recreatedWsCmd);
}

/**
 * Parse a workspace or window id: decimal digits only, not zero, and within 64 bits.
 * @param text the id as written in the snapshot
 * @param id receives the parsed id
 * @return true if the id is valid, false otherwise.
 */
bool parseId(const string &text, uint64_t &id) {
    if (text.empty()) return false;

    uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        uint64_t digit = ch - '0';
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value == 0) return false;

    id = value;
    return true;
}

/**
 * Decode a field written in the standard base64 alphabet. Its length must be a multiple of four
 * and '=' may only appear as one or two padding characters at the end.
 * @param encoded the field as written in the snapshot
 * @param decoded receives the decoded value
 * @return true if the field is well formed, false otherwise.
 */
bool decodeField(const string &encoded, string &decoded) {
    if (encoded.empty() || encoded.size() % 4 != 0) return false;

    size_t padding = 0;
    while (padding < 2 && encoded[encoded.size() - 1 - padding] == '=') padding++;

    for (size_t i = 0; i < encoded.size() - padding; i++) {
        char ch = encoded[i];
        bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
                     || ch == '+' || ch == '/';
        if (!valid) return false;
    }

    decoded = base64_decode(encoded);
    return true;
}

/**
 * Parse one snapshot line that is not blank.
 * @param line the line without its line feed and its final carriage return
 * @param entry receives the window described by the line
 * @param error receives the reason when the line is invalid
 * @param opts command line options; -r selects the raw form
 * @return true if the line is valid, false otherwise.
 */
bool parseLine(const string &line, SnapshotEntry &entry, string &error, const CommandLineOptions &opts) {
    // Output Name, Workspace Name, Workspace Id, Window Id, Window Name
    string fields[5];

    // Only a single carriage return ends a line.
    if (line.back() == '\r') {
        error = "more than one carriage return at the end of the line";
        return false;
    }

    if (opts.encodeStrings) {
        // Fields are separated by runs of spaces or tabs. An untitled window may have no fifth field.
        size_t count = 0;
        size_t start = line.find_first_not_of(" \t");
        while (start != string::npos) {
            size_t end = line.find_first_of(" \t", start);
            if (count < 5) fields[count] = line.substr(start, end == string::npos ? string::npos : end - start);
            count++;
            start = end == string::npos ? end : line.find_first_not_of(" \t", end);
        }
        if (count < 4 || count > 5) {
            error = "expected 4 or 5 fields, found " + to_string(count);
            return false;
        }
    } else {
        // The first four fields are not empty and each is followed by one space. The title is
        // everything after the fourth space; a line ending right after the window id has none.
        size_t start = 0;
        for (int i = 0; i < 4; i++) {
            size_t end = line.find(' ', start);
            if (end == string::npos && i < 3) end = start;
            if (end == string::npos) end = line.size();
            if (end == start) {
                error = "expected output, workspace, workspace id and window id, each followed by one space";
                return false;
            }
            fields[i] = line.substr(start, end - start);
            start = min(end + 1, line.size());
        }
        fields[4] = line.substr(start);
    }

    if (!parseId(fields[2], entry.workspaceId)) {
        error = "workspace id is not a nonzero decimal number of at most 64 bits";
        return false;
    }
    if (!parseId(fields[3], entry.windowId)) {
        error = "window id is not a nonzero decimal number of at most 64 bits";
        return false;
    }

    if (opts.encodeStrings) {
        if (!decodeField(fields[0], entry.outputName)) {
            error = "output name is not valid base64";
            return false;
        }
        if (!decodeField(fields[1], entry.workspaceName)) {
            error = "workspace name is not valid base64";
            return false;
        }
        if (!fields[4].empty() && !decodeField(fields[4], entry.windowTitle)) {
            error = "window title is not valid base64";
            return false;
        }
    } else {
        entry.outputName = fields[0];
        entry.workspaceName = fields[1];
        entry.windowTitle = fields[4];
    }

    if (entry.outputName.find('\0') != string::npos) {
        error = "output name contains a NUL character";
        return false;
    }
    if (entry.workspaceName.find('\0') != string::npos) {
        error = "workspace name contains a NUL character";
        return false;
    }
    if (entry.windowTitle.find('\0') != string::npos) {
        error = "window title contains a NUL character";
        return false;
    }

    return true;
}

/**
 * Read and check a whole snapshot. Every invalid line is reported on stderr.
 * @param input snapshot contents
 * @param entries receives the windows to restore, in file order
 * @param opts command line options; -r selects the raw form
 * @return true if every line is valid, false otherwise.
 */
bool parseSnapshot(const string &input, vector<SnapshotEntry> &entries, const CommandLineOptions &opts) {
    bool valid = true;
    size_t lineNumber = 0;
    size_t start = 0;

    // The final line does not need a closing newline.
    while (start < input.size()) {
        size_t end = input.find('\n', start);
        if (end == string::npos) end = input.size();
        string line = input.substr(start, end - start);
        start = end + 1;
        lineNumber++;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Blank lines are skipped but still counted.
        if (line.find_first_not_of(" \t") == string::npos) continue;

        SnapshotEntry entry;
        string error;
        if (parseLine(line, entry, error, opts)) {
            entries.push_back(entry);
        } else {
            cerr << "line " << lineNumber << ": " << error << endl;
            valid = false;
        }
    }

    return valid;
}

/**
 * Determine if input is being passed to program from a pipe.
 * This determines the mode of the program (read/write).
 * @return true if input is from terminal (meaning in write mode).
 */
bool inputFromTerminal() {
    return !isatty(fileno(stdin));
}

void printHelp() {
    cout
            << "Save and restore window containment in i3-wm.\n"
            << "Usage: i3-snapshot [-d | --debug] [-v | --verbose] [-c | --continue] [-r | --rawstrings] [-t | --title] [-o | --output] [-y | --dryrun]\n"
            << "-d: debug  -v: version  -c: ignore error  -r: raw strings  -t: match window title  -o: force output mode -y: dryrun\n"
            << "Generate a snapshot: i3-snapshot > snapshot.txt\n"
            << "Replay a snapshot: i3-snapshot < snapshot.txt"
            << endl;
}

void printVersion() {
    cout << "Version 0.1" << endl;
}

/**
 * Parse command-line options.
 * @param argc
 * @param argv
 * @return true for debug mode
 */
CommandLineOptions parseOptions(int argc, char **argv) {
    CommandLineOptions options{};

    options.debug = false;
    options.failFast = true;
    options.forceOutputMode = false;
    options.encodeStrings = true;
    options.dryRun = false;
    options.windowIdentifier = I3_ID;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printHelp();
            exit(0);
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printVersion();
            exit(0);
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--continue") == 0) {
            options.failFast = false;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            options.debug = true;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rawstrings") == 0) {
            options.encodeStrings = false;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--title") == 0) {
            options.windowIdentifier = WINDOW_TITLE;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            options.forceOutputMode = true;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--dryrun") == 0) {
            options.dryRun= true;
            options.debug = true;
        } else {
            cout << "Unrecognized command line option: '" << argv[i] << "'.  Aborting." << endl;
            exit(1);
        }
    }

    return options;
}

int main(int argc, char **argv) {
    CommandLineOptions opts = parseOptions(argc, argv);

    TreeState treeState;

    if (opts.forceOutputMode || !inputFromTerminal()) {
        findWindows(getTree(), treeState, opts);

        if (treeState.unsaveable) return 1;
        cout << treeState.snapshot.str();
    } else {
        // Check the whole snapshot before anything is sent to i3.
        string input((istreambuf_iterator<char>(cin)), istreambuf_iterator<char>());
        vector<SnapshotEntry> entries;

        if (!parseSnapshot(input, entries, opts)) return 1;
        if (entries.empty()) return 0;

        // A dry run only prints, so it needs neither X nor i3.
        unique_ptr<i3ipc::connection> i3connection;
        if (!opts.dryRun) i3connection.reset(new i3ipc::connection());

        for (const SnapshotEntry &entry : entries) {
            if (!moveWindow(i3connection.get(), entry.windowId, entry.outputName, entry.workspaceName,
                            entry.workspaceId, entry.windowTitle, opts)) {
                cerr << "Failed to move " << entry.windowId << " (" << entry.windowTitle << ")." << endl;

                if (opts.failFast) return 1;
            }
        }
    }

    return 0;
}
