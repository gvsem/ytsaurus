import requests

# Keep it sync with your pypi settings.
PYPI_URL = "https://pypi.yandex-team.ru/simple/{package_name}"

def get_package_versions(package_name, tag_filter_string=None):
    def extract_full_version_string(line):
        # Some magic to extract full version string from html link.
        return line.split(package_name.replace("-", "_"))[1].split("-", 1)[1].rsplit(".whl", 1)[0]

    def parts_to_version(parts):
        assert len(parts) == 4
        return "{}.{}.{}-{}".format(*parts)

    def extract_version(full_version_string):
        # Some magic to extract major.minor.patch version from full version string.
        return parts_to_version(full_version_string.replace("-", ".").replace("_", ".", 1).replace("post", "").split(".")[:4])

    rsp = requests.get(PYPI_URL.format(package_name=package_name))
    rsp.raise_for_status()

    html = rsp.text
    lines_with_download_links = [line for line in html.split("\n") if "rel=\"package\"" in line]
    full_version_strings = map(extract_full_version_string, lines_with_download_links)

    tag_filter = None
    if tag_filter_string is not None:
        tag_filter = lambda full_version_string: tag_filter_string in full_version_string
    fill_version_strings_filtered = filter(tag_filter, full_version_strings)

    return set(map(extract_version, fill_version_strings_filtered))
