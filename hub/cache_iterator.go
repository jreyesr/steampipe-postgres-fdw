package hub

import (
	"github.com/turbot/steampipe-postgres-fdw/hub/cache"
)

type cacheIterator struct {
	name   string
	rows   []map[string]interface{}
	index  int
	status queryStatus
}

func newCacheIterator(name string, cachedResult *cache.QueryResult) *cacheIterator {
	return &cacheIterator{
		name:   name,
		rows:   cachedResult.Rows,
		status: queryStatusReady,
	}
}

// ConnectionName implements Iterator
func (i *cacheIterator) ConnectionName() string {
	return i.name
}

func (i *cacheIterator) Status() queryStatus {
	return i.status
}

func (i *cacheIterator) Error() error {
	return nil
}

// Next implements Iterator
// return next row (tuple). Nil slice means there is no more rows to scan.
func (i *cacheIterator) Next() (map[string]interface{}, error) {
	if idx := i.index; idx < len(i.rows) {
		i.status = queryStatusStarted
		i.index++
		return i.rows[idx], nil
	}
	i.status = queryStatusComplete
	return nil, nil
}

// Close implements Iterator
// clear the rows and the index
func (i *cacheIterator) Close() {
	i.index = 0
	i.rows = nil
	i.status = queryStatusReady
}
