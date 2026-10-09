# Interactive experiences

Independent native macOS examples using SDL3 and Metal. Each example has its own source, dependencies, build script, and app bundle.

| Example | Description | Launch from this directory |
| --- | --- | --- |
| [Floating triangle](examples/floating-triangle/README.md) | The working triangle, with both a standard SDL GPU window and a transparent native Metal floating mode. | `./examples/floating-triangle/run.sh --floating` |
| [Fluid hose](examples/fluid-hose/README.md) | A desktop hose whose water collides with live window bounds and pools at the bottom. | `./examples/fluid-hose/run.sh` |

Build an example by running its `build.sh`. Launch scripts and app resources resolve paths relative to their project, so you can run them from this directory or from inside the example folder.

The cached SDL SDK is project-local; no global packages are required. Xcode and its Metal tools are required to rebuild.
