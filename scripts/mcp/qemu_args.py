import pathlib


def device_args(image):
    build_dir = pathlib.Path(image).resolve().parent.parent
    config = build_dir / "qemu-device-args.txt"
    if not config.is_file():
        raise RuntimeError(f"configure the image build first: missing {config}")
    return config.read_text().splitlines()
