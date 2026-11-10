Place markdown files in here to be viewed on the radio.

To download the README.md from every app folder in the UV-K1/K5 v3 firmware
repository, run:

    python3 "markdown files/download_armel_app_readmes.py"

Each file is saved as <folder>.md. The script reports folders without a README.md, and asks before overwriting an existing file with different contents. The top-level App/apps/README.md is not downloaded, if it ever exists.
