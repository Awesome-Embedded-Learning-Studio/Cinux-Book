import pathlib


def device_args(image, config_name="qemu-device-args.txt"):
    build_dir = pathlib.Path(image).resolve().parent.parent
    config = build_dir / config_name
    if not config.is_file():
        raise RuntimeError(f"configure the image build first: missing {config}")
    return config.read_text().splitlines()
