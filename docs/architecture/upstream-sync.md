git fetch upstream
git checkout upstream-sync
git merge --ff-only upstream/master
git push
git checkout main
git merge upstream-sync