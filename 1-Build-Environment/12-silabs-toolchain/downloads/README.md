# Silicon Labs archives downloaded by hand

`install_silabs.sh` and the `Dockerfile` download two archives from silabs.com:

| File name (keep it as is) | Download page |
|---|---|
| `slc_cli_linux.zip` | https://www.silabs.com/documents/public/software/slc_cli_linux.zip |
| `SimplicityCommander-Linux.zip` | https://www.silabs.com/documents/public/software/SimplicityCommander-Linux.zip |

Since 2026-10-22 silabs.com answers `403 Forbidden` to command-line clients
(`curl`, `wget`), while the same links still work in a web browser. When the
download fails, both installers stop and point here.

To work around it, open each link in a browser and save the file in this
directory under the name shown above, then run the installer again:

- native: `sudo ./install_deps.sh` (or `12-silabs-toolchain/install_silabs.sh`);
- Docker: `docker build -t rtl8196e-gateway-builder .`

A file present here is always used instead of the download, so delete it to go
back to downloading. The archives are not tracked by git (see `.gitignore`), and
the Docker build reads them through a build-time mount, so they do not end up in
an image layer.
