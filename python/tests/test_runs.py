from animus.runs import ARCHIVE_DIR, archive_run


def test_an_earlier_run_is_archived(tmp_path):
    run_dir = tmp_path / "warrior_dps"
    run_dir.mkdir()
    (run_dir / "latest.pt").write_text("old")

    archived = archive_run(run_dir)

    assert archived is not None and (archived / "latest.pt").read_text() == "old"
    assert archived.parent == tmp_path / ARCHIVE_DIR
    assert run_dir.is_dir() and not any(run_dir.iterdir())

    # Every start archives again: nothing is resumed.
    (run_dir / "latest.pt").write_text("new")
    second = archive_run(run_dir)
    assert second is not None and second != archived and (second / "latest.pt").read_text() == "new"


def test_nothing_to_archive(tmp_path):
    run_dir = tmp_path / "mage_dps_duel"

    assert archive_run(run_dir) is None
    assert run_dir.is_dir()
    assert not (tmp_path / ARCHIVE_DIR).exists()
