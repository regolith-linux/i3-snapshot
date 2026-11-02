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
#include <cstring>
#include <zconf.h>
#include <iomanip>

#include "base64.h"

using namespace std;

/**
 * Keep track of output and workspace as the i3 container tree is traversed depth-first.
 */
struct TreeState {
    string outputName;
    string workspaceName;
    size_t workspaceId{};
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
 * @return the GET_TREE reply
 */
Json::Value getTree() {
    int32_t sockfd = i3ipc::i3_connect(i3ipc::get_socketpath());
    auto buf = i3ipc::i3_msg(sockfd, i3ipc::ClientMessageType::GET_TREE);
    i3ipc::i3_disconnect(sockfd);

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(string(buf->payload, buf->header->size), root, false)) {
        cerr << "Failed to parse the i3 tree: " << reader.getFormattedErrorMessages() << endl;
        exit(1);
    }
    return root;
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
        cout << outputEncoded << " " << workspaceEncoded << " " <<  treeState.workspaceId << " " << c["id"].asUInt64() << " "
             << windowEncoded << endl;
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
 * Move a workspace to an out and a window to a workspace.
 * @param i3conn i3 connection
 * @param windowId i3 window id
 * @param outputName system name for output (monitor)
 * @param workspaceName i3 name for workspace
 * @param workspaceId i3 id for workspace
 * @param windowTitle window title
 * @return true if operation success, false otherwise.
 */
bool
moveWindow(const i3ipc::connection &i3conn, size_t windowId, const string &outputName, const string &workspaceName,
           size_t workspaceId, const string &windowTitle, CommandLineOptions &opts) {
    // Move workspace to output
    // i3-msg [workspace=" 2 <span foreground='#2aa198'></span> "] move workspace to output "eDP-1"
    string wsCmd;
    if (opts.windowIdentifier == I3_ID) {
        wsCmd = "[con_id=" + to_string(workspaceId) + "] move workspace to output " + outputName;
    } else {
        wsCmd = "[workspace=\"" + workspaceName + "\"] move workspace to output " + outputName;
    }

    if (opts.debug) cout << "i3-msg " << wsCmd << endl;

    // This fails when the workspace no longer exists. Moving the window
    // below creates it again, and the workspace is then moved through the
    // window.
    bool workspaceMoved = opts.dryRun || i3conn.send_command(wsCmd);

    // Move window to workspace
    string windowCmd;
    // https://build.i3wm.org/docs/userguide.html#command_criteria
    if (opts.windowIdentifier == I3_ID) {
        windowCmd = "[con_id=" + to_string(windowId) + "] move container to workspace " + workspaceName;
    } else {
        windowCmd = "[title=\"" + windowTitle + "\"] move container to workspace " + workspaceName;
    }

    if (opts.debug) cout << "i3-msg " << windowCmd << endl;

    if (opts.dryRun) return true;

    if (!i3conn.send_command(windowCmd)) return false;
    if (workspaceMoved) return true;

    string windowCriteria;
    if (opts.windowIdentifier == I3_ID) {
        windowCriteria = "[con_id=" + to_string(windowId) + "]";
    } else {
        windowCriteria = "[title=\"" + windowTitle + "\"]";
    }
    string recreatedWsCmd = windowCriteria + " move workspace to output " + outputName;

    if (opts.debug) cout << "i3-msg " << recreatedWsCmd << endl;

    return i3conn.send_command(recreatedWsCmd);
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

    i3ipc::connection i3connection;
    TreeState treeState;

    if (opts.forceOutputMode || !inputFromTerminal()) {
        findWindows(getTree(), treeState, opts);
    } else {
        string outputNameEnc, workspaceNameEnc, workspaceIdStr, windowIdStr, windowNameEnc;

        while (!cin.eof()) {
            cin >> outputNameEnc >> workspaceNameEnc >> workspaceIdStr >> windowIdStr >> windowNameEnc;

            string outputName = base64_decode(outputNameEnc);
            string workspaceName = base64_decode(workspaceNameEnc);
            size_t workspaceId = stoul(workspaceIdStr);
            string windowName = base64_decode(windowNameEnc);
            size_t windowId = stoul(windowIdStr);

            std::stringstream escapedWorkspaceName;
            escapedWorkspaceName << std::quoted(workspaceName);

            if (!moveWindow(i3connection, windowId, outputName, escapedWorkspaceName.str(), workspaceId, windowName, opts)) {
                cerr << "Failed to move " << windowId << " (" << windowName << ")." << endl;

                if (opts.failFast) return 1;
            }
        }
    }

    return 0;
}
