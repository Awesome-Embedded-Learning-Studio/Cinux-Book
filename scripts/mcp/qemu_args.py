import pathlib


def qemu_command(image):
    config = pathlib.Path(image).resolve().parent.parent / 'qemu-command.txt'
    if not config.is_file():
        return ['qemu-system-x86_64']
    command = config.read_text().splitlines()
    if not command or any(not argument for argument in command):
        raise RuntimeError(f'invalid QEMU command: {config}')
    return command


def device_args(image, config_name="qemu-device-args.txt"):
    build_dir = pathlib.Path(image).resolve().parent.parent
    config = build_dir / config_name
    if not config.is_file():
        raise RuntimeError(f"configure the image build first: missing {config}")
    return config.read_text().splitlines()
