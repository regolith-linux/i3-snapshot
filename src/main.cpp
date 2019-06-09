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
#include <cstring>
#include <zconf.h>

#include "base64.h"

using namespace std;

/**
 * Keep track of output and workspace as the i3 container tree is traversed depth-first.
 */
struct TreeState {
    string outputName;
    string workspaceName;
    size_t workspaceId = -1;
};

/**
 * Determine if the i3 container is a window type.
 * @param c i3 container
 * @return true if container is window, false otherwise.
 */
bool isWindow(const i3ipc::container_t &c) {
    return c.type == "con" && c.xwindow_id != 0;
}

/**
 * Determine if container should not be ignored.
 * @param c i3 container
 * @return true if container is valid, false otherwise.
 */
bool isValidParent(const i3ipc::container_t &c) {
    return c.type != "dockarea";
}

/**
 * Traverse i3 containers and emit relevant info to stdout.
 *
 * @param c i3 container
 * @param treeState storage of current state of tree traversal.
 */
void findWindows(const i3ipc::container_t &c, TreeState &treeState) {
    if (c.type == "output") {
        treeState.outputName = c.name;
    } else if (c.type == "workspace") {
        treeState.workspaceId = c.id;
        treeState.workspaceName = c.name;
    } else if (isWindow(c)) {
        if (treeState.outputName.empty() || treeState.workspaceName.empty() || treeState.workspaceId == -1) {
            cout << "Invalid tree state, aborting." << endl;
            exit(1);
        }

        string outputEncoded = base64_encode(reinterpret_cast<const unsigned char *>(treeState.outputName.c_str()),
                                                  treeState.outputName.length());
        string workspaceEncoded = base64_encode(
                reinterpret_cast<const unsigned char *>(treeState.workspaceName.c_str()),
                treeState.workspaceName.length());
        string escapedName = c.name;
        transform(escapedName.begin(), escapedName.end(), escapedName.begin(), [](char ch) {
            return ch == ' ' ? '_' : ch;
        });

        // Output Name, Workspace Name, Workspace Id, Window Id, Window Name
        cout << outputEncoded << " " << workspaceEncoded << " " << treeState.workspaceId << " " << c.id << " "
                  << escapedName << endl;
    }

    if (isValidParent(c))
        for (auto &node : c.nodes)
            findWindows(*node, treeState);
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
           const size_t &workspaceId, const string &windowTitle, bool &debug) {
    // Move workspace to output
    // i3-msg '[workspace=" 2 <span foreground='#2aa198'></span> "]' move workspace to output "eDP-1"
    string wsCmd = "[con_id=" + to_string(workspaceId) + "] move workspace to output " + outputName;
    if (debug) cout << "i3-msg " << wsCmd << endl;

    if (!i3conn.send_command(wsCmd)) return false;

    // Move window to workspace
    string windowCmd =
            "[con_id=" + to_string(windowId) + "] move container to workspace \"" + workspaceName + "\"";

    if (debug) cout << "i3-msg " << windowCmd << endl;

    return i3conn.send_command(windowCmd);
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
            << "Usage: i3-snapshot [-d] \n"
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
bool parseOptions(int argc, char **argv) {
    if (argc == 2 && ((strncmp(argv[1], "-h", 2) == 0) || (strncmp(argv[1], "--help", 6) == 0))) {
        printHelp();
        exit(0);
    } else if (argc == 2 && ((strncmp(argv[1], "-v", 2) == 0) || (strncmp(argv[1], "--version", 8) == 0))) {
        printVersion();
        exit(0);
    } else if (argc == 2 && (strncmp(argv[1], "-d", 2) == 0)) {
        return true;
    }

    return false;
}

int main(int argc, char **argv) {
    bool debug = parseOptions(argc, argv);

    i3ipc::connection i3connection;
    TreeState treeState;

    if (!inputFromTerminal()) {
        findWindows(*i3connection.get_tree(), treeState);
    } else {
        string outputNameEnc, workspaceNameEnc, workspaceIdStr, windowIdStr, windowName;

        while (!cin.eof()) {
            cin >> outputNameEnc >> workspaceNameEnc >> workspaceIdStr >> windowIdStr >> windowName;

            string outputName = base64_decode(outputNameEnc);
            string workspaceName = base64_decode(workspaceNameEnc);
            size_t workspaceId = stoul(workspaceIdStr);
            size_t windowId = stoul(windowIdStr);

            if (!moveWindow(i3connection, windowId, outputName, workspaceName, workspaceId, windowName, debug)) {
                cerr << "Failed to move " << windowId << " (" << windowName << ").  Aborting." << endl;
                return 1;
            }
        }
    }

    return 0;
}

