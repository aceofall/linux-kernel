/*
 * linux/mm/mmzone.c
 *
 * management codes for pgdats, zones and page flags
 */


#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/mmzone.h>

// ARM10C 20140329
struct pglist_data *first_online_pgdat(void)
{
	// first_online_node: 0
	return NODE_DATA(first_online_node);
	// return &contig_page_data
}

// ARM10C 20140329
// pgdat: &contig_page_data
struct pglist_data *next_online_pgdat(struct pglist_data *pgdat)
{
	// pgdat->node_id: (&contig_page_data)->node_id: 0
	// next_online_node((&contig_page_data)->node_id): 1
	int nid = next_online_node(pgdat->node_id);
	// nid: 1

	// MAX_NUMNODES: 1
	if (nid == MAX_NUMNODES)
		return NULL;
		// return NULL

	return NODE_DATA(nid);
}

/*
 * next_zone - helper magic for for_each_zone()
 */
struct zone *next_zone(struct zone *zone)
{
	pg_data_t *pgdat = zone->zone_pgdat;

	if (zone < pgdat->node_zones + MAX_NR_ZONES - 1)
		zone++;
	else {
		pgdat = next_online_pgdat(pgdat);
		if (pgdat)
			zone = pgdat->node_zones;
		else
			zone = NULL;
	}
	return zone;
}

static inline int zref_in_nodemask(struct zoneref *zref, nodemask_t *nodes)
{
#ifdef CONFIG_NUMA
	return node_isset(zonelist_node_idx(zref), *nodes);
#else
	return 1;
#endif /* CONFIG_NUMA */
}

/* Returns the next zone at or below highest_zoneidx in a zonelist */
// ARM10C 20140308
// zonelist->_zonerefs: contig_page_data->node_zonelists->_zonerefs
// highest_zoneidx: 0, nodes: 0, &zone
// ARM10C 20140426
// zonelist->_zonerefs: contig_page_data->node_zonelists->_zonerefs
// highest_zoneidx: 0, nodes: &node_states[N_HIGH_MEMORY], zone: &preferred_zone
struct zoneref *next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes,
					struct zone **zone)
{
	/*
	 * Find the next suitable zone to use for the allocation.
	 * Only filter based on nodemask if it's set
	 */
	// nodes: 0
	// ARM10C 20140426
	// nodes: &node_states[N_HIGH_MEMORY]
	if (likely(nodes == NULL))
		// z: contig_page_data->node_zonelists->_zonerefs[0], highest_zoneidx: 0
		// z: contig_page_data->node_zonelists->_zonerefs[1], highest_zoneidx: 0
		// [2nd] zonelist_zone_idx(z): 0
		while (zonelist_zone_idx(z) > highest_zoneidx)
			// [1st] zonelist_zone_idx(z): 1
			z++;
			// [1st] z: contig_page_data->node_zonelists->_zonerefs[1]
	else
		// ARM10C 20140426
		// z: contig_page_data->node_zonelists->_zonerefs, zonelist_zone_idx(z): 1
		// highest_zoneidx: 0
		while (zonelist_zone_idx(z) > highest_zoneidx ||
				(z->zone && !zref_in_nodemask(z, nodes)))
			z++;

	// z: contig_page_data->node_zonelists->_zonerefs[1]
	*zone = zonelist_zone(z);
	// zone: contig_page_data->node_zones[0]

	// z: contig_page_data->node_zonelists->_zonerefs[1]
	return z;
	// return contig_page_data->node_zonelists->_zonerefs[1]
}

#ifdef CONFIG_ARCH_HAS_HOLES_MEMORYMODEL
int memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone)
{
	if (page_to_pfn(page) != pfn)
		return 0;

	if (page_zone(page) != zone)
		return 0;

	return 1;
}
#endif /* CONFIG_ARCH_HAS_HOLES_MEMORYMODEL */

// ARM10C 20140111
// least recently used vector init 
void lruvec_init(struct lruvec *lruvec)
{
	enum lru_list lru;

	memset(lruvec, 0, sizeof(struct lruvec));

	for_each_lru(lru)
		INIT_LIST_HEAD(&lruvec->lists[lru]);
}

#if defined(CONFIG_NUMA_BALANCING) && !defined(LAST_CPUPID_NOT_IN_PAGE_FLAGS)
int page_cpupid_xchg_last(struct page *page, int cpupid)
{
	unsigned long old_flags, flags;
	int last_cpupid;

	do {
		old_flags = flags = page->flags;
		last_cpupid = page_cpupid_last(page);

		flags &= ~(LAST_CPUPID_MASK << LAST_CPUPID_PGSHIFT);
		flags |= (cpupid & LAST_CPUPID_MASK) << LAST_CPUPID_PGSHIFT;
	} while (unlikely(cmpxchg(&page->flags, old_flags, flags) != old_flags));

	return last_cpupid;
}
#endif
