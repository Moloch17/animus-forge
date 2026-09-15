from animus.runs import ARCHIVE_DIR, archive_run, prune_checkpoints


def test_only_the_newest_checkpoints_are_kept(tmp_path):
    for update in (25, 50, 75, 100, 125):
        (tmp_path / f"checkpoint_{update:06d}.pt").write_text("x")
    (tmp_path / "latest.pt").write_text("x")
    (tmp_path / "best.pt").write_text("x")

    removed = prune_checkpoints(tmp_path, 2)

    assert sorted(p.name for p in removed) == ["checkpoint_000025.pt", "checkpoint_000050.pt", "checkpoint_000075.pt"]
    assert sorted(p.name for p in tmp_path.iterdir()) == ["best.pt", "checkpoint_000100.pt", "checkpoint_000125.pt",
                                                          "latest.pt"]
    assert prune_checkpoints(tmp_path, 0) == []


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
