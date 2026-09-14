from animus.runs import ARCHIVE_DIR, CLEAN_RUN_MARKER, start_clean_run


def test_clean_run_archives_the_old_run_once(tmp_path):
    run_dir = tmp_path / "warrior_dps"
    run_dir.mkdir()
    (run_dir / "latest.pt").write_text("old")

    archived = start_clean_run(run_dir, "pilot-1")

    assert archived is not None and (archived / "latest.pt").read_text() == "old"
    assert archived.parent == tmp_path / ARCHIVE_DIR
    assert not (run_dir / "latest.pt").exists()
    assert (run_dir / CLEAN_RUN_MARKER).read_text().strip() == "pilot-1"

    # A restart under the same id keeps the clean run's progress.
    (run_dir / "latest.pt").write_text("new")
    assert start_clean_run(run_dir, "pilot-1") is None
    assert (run_dir / "latest.pt").read_text() == "new"

    # A new id starts over again.
    assert start_clean_run(run_dir, "pilot-2") is not None
    assert not (run_dir / "latest.pt").exists()


def test_clean_run_without_an_earlier_run(tmp_path):
    run_dir = tmp_path / "mage_dps_duel"

    assert start_clean_run(run_dir, "a") is None
    assert (run_dir / CLEAN_RUN_MARKER).exists()
    assert not (tmp_path / ARCHIVE_DIR).exists()
