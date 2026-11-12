# Pinned sources of the 3DS release build, read by ci.sh: name, URL and SHA-256 per line.
SOURCES="
SDL3-3.4.18.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
makerom-v0.19.0-ubuntu_x86_64.zip https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.19.0/makerom-v0.19.0-ubuntu_x86_64.zip 287b809dec064e0ad597e3d272c49ecb7eed41693d5ee6fef9d8a8aa24c2497e
bannertool-1.2.3-linux.tar.gz https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/bannertool-1.2.3-linux.tar.gz 748519d200519db18e9fd00c332ac32f5411e41230258291e89953a76c1f7155
"
# OpenDSP has no source archive with a published hash: a commit of its repository.
OPENDSP_REPO=https://github.com/Cellenseres/OpenDSP
OPENDSP_COMMIT=8c8b308d3005de2ed340df42bd819c234962d690
