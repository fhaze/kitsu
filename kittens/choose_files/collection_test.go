package choose_files

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func sorted_results_of(scores ...[]int) *SortedResults {
	r := NewSortedResults()
	for _, sl := range scores {
		items := make([]*ResultItem, len(sl))
		for i, x := range sl {
			items[i] = &ResultItem{text: fmt.Sprint(x), score: CombinedScore(x)}
		}
		r.AddSortedSlice(items)
	}
	return r
}

func TestSortedResultsClosest(t *testing.T) {
	r := sorted_results_of([]int{10, 20, 30}, []int{40, 50, 60}, []int{70, 80, 90})
	for _, tc := range []struct {
		from     CollectionIndex
		wanted   []int
		expected *CollectionIndex
	}{
		{CollectionIndex{1, 1}, []int{80}, &CollectionIndex{2, 1}},
		{CollectionIndex{1, 1}, []int{20}, &CollectionIndex{0, 1}},
		{CollectionIndex{1, 1}, []int{20, 70}, &CollectionIndex{2, 0}},
		{CollectionIndex{1, 1}, []int{10, 70}, &CollectionIndex{2, 0}},
		{CollectionIndex{1, 1}, []int{30, 80}, &CollectionIndex{0, 2}},
		{CollectionIndex{1, 1}, []int{30, 70}, &CollectionIndex{2, 0}}, // ties go forward
		{CollectionIndex{1, 1}, []int{50}, nil},
		{CollectionIndex{0, 0}, []int{90}, &CollectionIndex{2, 2}},
	} {
		for range 50 { // it used to race
			actual := r.Closest(tc.from, func(x *ResultItem) bool {
				for _, w := range tc.wanted {
					if int(x.score) == w {
						return true
					}
				}
				return false
			})
			if (actual == nil) != (tc.expected == nil) || (actual != nil && *actual != *tc.expected) {
				t.Fatalf("closest to %v matching %v: expected %v got %v", tc.from, tc.wanted, tc.expected, actual)
			}
		}
	}
}

func TestSortedResultsWithStaleIndex(t *testing.T) {
	empty := NewSortedResults()
	if idx := empty.IncrementIndexWithWrapAround(CollectionIndex{}, -1); idx != (CollectionIndex{}) {
		t.Fatalf("moving to the last of no results gave: %v", idx)
	}
	if idx := empty.IncrementIndexWithWrapAround(CollectionIndex{}, 1); idx != (CollectionIndex{}) {
		t.Fatalf("moving to the next of no results gave: %v", idx)
	}
	// an index from before the results were recomputed, after toggling dotfiles, say
	r := sorted_results_of([]int{10, 20, 30})
	stale := CollectionIndex{Slice: 4, Pos: 2}
	if idx := r.IncrementIndexWithWrapAround(stale, -2); idx != (CollectionIndex{0, 0}) {
		t.Fatalf("decrementing a stale index gave: %v", idx)
	}
	cols, _, _ := r.SplitIntoColumns(func(int) int { return 1 }, 2, 5, stale)
	if len(cols) != 1 || len(cols[0]) != 2 {
		t.Fatalf("splitting with a stale index gave: %v", cols)
	}
}

func TestChooseFilesQueryChangeDuringScan(t *testing.T) {
	const num_dirs, files_per_dir = 5, 10
	root := node{name: string(os.PathSeparator), children: map[string]*node{}}
	for d := range num_dirs {
		dir := &node{name: fmt.Sprintf("d%d", d), children: map[string]*node{}}
		for f := range files_per_dir {
			name := fmt.Sprintf("f%d", f)
			dir.children[name] = &node{name: name}
		}
		root.children[dir.name] = dir
	}
	gate := make(chan struct{})
	first_batch, done := make(chan struct{}), make(chan struct{})
	var signalled bool
	s := NewFileSystemScorer(string(os.PathSeparator), "", Filter{}, false, func(err error, is_complete bool) {
		if err != nil {
			t.Error(err)
		}
		if is_complete {
			close(done)
		} else if !signalled {
			signalled = true
			close(first_batch)
		}
	})
	s.dir_reader = func(path string) ([]fs.DirEntry, error) {
		if filepath.Clean(strings.TrimPrefix(path, filepath.VolumeName(path))) != string(os.PathSeparator) {
			<-gate // the sub-directories are slow to read
		}
		return root.ReadDir(path)
	}
	s.Start()
	select {
	case <-first_batch:
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for the first batch")
	}
	changed := make(chan struct{})
	go func() { s.Change_query("f"); close(changed) }()
	select {
	case <-changed:
	case <-time.After(5 * time.Second):
		close(gate)
		t.Fatal("changing the query waited for a slow directory to be read")
	}
	close(gate)
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for the scan to finish")
	}
	rr, _ := s.Results()
	if n := rr.Len(); n != num_dirs*files_per_dir {
		t.Fatalf("changing the query during the scan stopped it: %d of %d files found", n, num_dirs*files_per_dir)
	}
}
