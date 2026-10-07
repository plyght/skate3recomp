# rexglue SDK patches

`rexglue-sdk-ios.patch` carries the SDK-side changes for the iOS port
(platform macro, arm64 fiber switch, memory layout, SDL/UIKit glue, touch
input, power/frame pacing). It applies on top of the pinned
`third_party/rexglue-sdk` commit and is applied automatically by
`ios/apply-sdk-patches.sh` (also called by the iOS build script).

To apply by hand:

```sh
git -C third_party/rexglue-sdk apply ../../patches/rexglue-sdk-ios.patch
```

To regenerate after editing the SDK checkout (submodule pointers must stay out
of the patch):

```sh
git -C third_party/rexglue-sdk diff --ignore-submodules=all > patches/rexglue-sdk-ios.patch
```
