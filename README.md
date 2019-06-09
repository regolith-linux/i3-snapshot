# i3-snapshot
Save and restore window and workspace layout within an i3wm instance.

## Usage

i3-snapshot has two modes: 

* record: read i3 current state and emit to stdout
`i3-snapshot > snapshot.txt`
* restore: modify the i3 window state from a previous configuration.
`i3-snapshot < snapshot.txt`
  
## Notes

This program is intended to be used when window layouts are mangled by display hotplug events or other changes that cause i3wm to restructure the containment layout.

i3-snapshot employs a 'best effort' and 'fail-fast' strategy.  This means that it does little validation and aborts execution upon any failure.

The output is meant to be somewhat human readable for basic troubleshooting purposes.

i3-snapshot is not an alternative to i3-save-tree.  i3-save-tree is for long-lived workspace structures that are to be populated by users interactively.  i3-snapshot only works within a single i3wm instance because it uses the internal ids to reference specific windows.  This means that a snapshot cannot be used after the i3wm session it was recorded in exits. 

## Example

To save your current window and workspace layout to a file:
```
$ i3-snapshot > layout.txt
```

To restore a layout from a file:
```
$ i3-snapshot < layout.txt
```

## How to build

```
$ git clone https://github.com/regolith-linux/i3-snapshot.git
...
$ cd i3-snapshot
...
$ mkdir build && cd build
$ cmake ..
$ make
...
$ ./i3-snapshot
```

## Expectation of quality

i3-snapshot should be considered experimental.