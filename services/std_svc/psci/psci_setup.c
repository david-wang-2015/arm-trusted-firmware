/*
 * Copyright (c) 2013-2015, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arch.h>
#include <arch_helpers.h>
#include <assert.h>
#include <bl_common.h>
#include <context.h>
#include <context_mgmt.h>
#include <platform.h>
#include <stddef.h>
#include "psci_private.h"

/*******************************************************************************
 * Per cpu non-secure contexts used to program the architectural state prior
 * return to the normal world.
 * TODO: Use the memory allocator to set aside memory for the contexts instead
 * of relying on platform defined constants. Using PSCI_NUM_PWR_DOMAINS will be
 * an overkill.
 ******************************************************************************/
static cpu_context_t psci_ns_context[PLATFORM_CORE_COUNT];

/******************************************************************************
 * Define the psci capability variable.
 *****************************************************************************/
uint32_t psci_caps;

/*******************************************************************************
 * Function which initializes the 'psci_non_cpu_pd_nodes' or the
 * 'psci_cpu_pd_nodes' corresponding to the power level.
 ******************************************************************************/
static void psci_init_pwr_domain_node(int array_idx, int parent_idx, int level)
{
	if (level > PSCI_CPU_PWR_LVL) {
		psci_non_cpu_pd_nodes[array_idx].level = level;
		psci_lock_init(psci_non_cpu_pd_nodes, array_idx);
		psci_non_cpu_pd_nodes[array_idx].parent_node = parent_idx;

#if !USE_COHERENT_MEM
		flush_dcache_range((uint64_t) &psci_non_cpu_pd_nodes[array_idx],
				   sizeof(psci_non_cpu_pd_nodes[array_idx]));
#endif
	} else {

		psci_cpu_pd_nodes[array_idx].parent_node = parent_idx;

		/* Initialize with an invalid mpidr */
		psci_cpu_pd_nodes[array_idx].mpidr = -1;

		/*
		 * Mark the cpu as OFF. Higher power level reference counts
		 * have already been memset to 0
		 */
		set_cpu_data_by_index(array_idx,
				      psci_svc_cpu_data.psci_state,
				      PSCI_STATE_OFF);

		/* Invalidate the suspend context for the node */
		set_cpu_data_by_index(array_idx,
				      psci_svc_cpu_data.power_state,
				      PSCI_INVALID_DATA);

		flush_cpu_data_by_index(array_idx, psci_svc_cpu_data);

		cm_set_context_by_index(array_idx,
					(void *) &psci_ns_context[array_idx],
					NON_SECURE);
	}
}

/*******************************************************************************
 * This functions updates cpu_start_idx and ncpus field for each of the node in
 * psci_non_cpu_pd_nodes[]. It does so by comparing the parent nodes of each of
 * the CPUs and check whether they match with the parent of the previous
 * CPU. The basic assumption for this work is that children of the same parent
 * are allocated adjacent indices. The platform should ensure this though proper
 * mapping of the CPUs to indices via platform_get_core_pos() and
 * platform_my_core_pos() APIs.
 *******************************************************************************/
void psci_update_pwrlvl_limits()
{
	int cpu_idx, j;
	unsigned int nodes_idx[PLAT_MAX_PWR_LVL] = {0};
	unsigned int temp_index[PLAT_MAX_PWR_LVL];

	for (cpu_idx = 0; cpu_idx < PLATFORM_CORE_COUNT; cpu_idx++) {
		psci_get_parent_pwr_domain_nodes(cpu_idx,
						 PLAT_MAX_PWR_LVL,
						 temp_index);
		for (j = PLAT_MAX_PWR_LVL - 1; j >= 0; j--) {
			if (temp_index[j] != nodes_idx[j]) {
				nodes_idx[j] = temp_index[j];
				psci_non_cpu_pd_nodes[nodes_idx[j]].cpu_start_idx
					= cpu_idx;
			}
			psci_non_cpu_pd_nodes[nodes_idx[j]].ncpus++;
		}
	}
}

/*******************************************************************************
 * Core routine to populate the power domain tree. The tree descriptor passed by
 * the platform is populated breadth-first and the first entry in the map
 * informs the number of root power domains. The parent nodes of the root nodes
 * will point to an invalid entry(-1).
 ******************************************************************************/
void populate_power_domain_tree(unsigned char *plat_array,
				int num_levels)
{
	uint32_t i, j = 0, num_nodes_at_lvl = 1, num_nodes_at_next_lvl;
	uint32_t node_index = 0, parent_node_index = 0, num_children;

	/*
	 * For each level the inputs are:
	 * - number of nodes at this level in plat_array i.e. num_nodes_at_level
	 *   This is the sum of values of nodes at the parent level.
	 * - Index of first entry at this level in the plat_array i.e.
	 *   parent_node_index.
	 * - Index of first free entry in psci_non_cpu_pd_nodes[] or
	 *   psci_cpu_pd_nodes[] i.e. node_index depending upon the level.
	 */
	while (num_levels >= 0) {
		num_nodes_at_next_lvl = 0;
		/*
		 * For each entry (parent node) at this level in the plat_array:
		 * - Find the number of children
		 * - Allocate a node in a power domain array for each child
		 * - Set the parent of the child to the parent_node_index - 1
		 * - Increment parent_node_index to point to the next parent
		 * - Accumulate the number of children at next level.
		 */
		for (i = 0; i < num_nodes_at_lvl; i++) {
			assert(parent_node_index <=
			       PSCI_NUM_PWR_DOMAINS - PLATFORM_CORE_COUNT);
			num_children = plat_array[parent_node_index];

			for (j = node_index;
				j < node_index + num_children; j++)
				psci_init_pwr_domain_node(j,
							  parent_node_index - 1,
							  num_levels);

			node_index = j;
			num_nodes_at_next_lvl += num_children;
			parent_node_index++;
		}

		num_nodes_at_lvl = num_nodes_at_next_lvl;
		num_levels--;

		/* Reset the index for the cpu power domain array */
		if (num_levels == 0)
			node_index = 0;
	}

	/* Validate the sanity of array exported by the platform */
	assert(j == PLATFORM_CORE_COUNT);
}

/*******************************************************************************
 * This function initializes the power domain topology tree by querying the
 * platform. The power domain nodes higher than the CPU are populated in the
 * array psci_non_cpu_pd_nodes[] and the CPU power domains are populated in
 * psci_cpu_pd_nodes[]. The platform exports its static topology map through the
 * populate_power_domain_topology_tree() API. The algorithm populates the
 * psci_non_cpu_pd_nodes and psci_cpu_pd_nodes iteratively by using this
 * topology map.  On a platform that implements two clusters of 2 cpus each, and
 * supporting 3 domain levels, the populated psci_non_cpu_pd_nodes would look
 * like this:
 *
 * ---------------------------------------------------
 * | system node | cluster 0 node  | cluster 1 node  |
 * ---------------------------------------------------
 *
 * And populated psci_cpu_pd_nodes would look like this :
 * <-    cpus cluster0   -><-   cpus cluster1   ->
 * ------------------------------------------------
 * |   CPU 0   |   CPU 1   |   CPU 2   |   CPU 3  |
 * ------------------------------------------------
 ******************************************************************************/
int32_t psci_setup(void)
{
	unsigned char *topology_tree;
	psci_plat_pm_ops = NULL;

	/* Query the topology map from the platform */
	topology_tree = platform_get_power_domain_tree_desc();

	/* Populate the power domain arrays using the platform topology map */
	populate_power_domain_tree(topology_tree, PLAT_MAX_PWR_LVL);

	/* Update the CPU limits for each node in psci_non_cpu_pd_nodes */
	psci_update_pwrlvl_limits();

	/* Populate the mpidr field of cpu node for this CPU */
	psci_cpu_pd_nodes[platform_my_core_pos()].mpidr =
		read_mpidr() & MPIDR_AFFINITY_MASK;

#if !USE_COHERENT_MEM
	/*
	 * The psci_non_cpu_pd_nodes only needs flushing when it's not allocated in
	 * coherent memory.
	 */
	flush_dcache_range((uint64_t) &psci_non_cpu_pd_nodes,
			   sizeof(psci_non_cpu_pd_nodes));
#endif

	flush_dcache_range((uint64_t) &psci_cpu_pd_nodes,
			   sizeof(psci_cpu_pd_nodes));

	/*
	 * Mark the current CPU and its parent power domains as ON. No need to lock
	 * as this is the primary cpu.
	 */
	psci_do_state_coordination(PLAT_MAX_PWR_LVL, platform_my_core_pos(),
				   PSCI_STATE_ON);

	platform_setup_pm(&psci_plat_pm_ops);
	assert(psci_plat_pm_ops);

	/* Initialize the psci capability */
	psci_caps = PSCI_GENERIC_CAP;

	if (psci_plat_pm_ops->pwr_domain_off)
		psci_caps |=  define_psci_cap(PSCI_CPU_OFF);
	if (psci_plat_pm_ops->pwr_domain_on &&
			psci_plat_pm_ops->pwr_domain_on_finish)
		psci_caps |=  define_psci_cap(PSCI_CPU_ON_AARCH64);
	if (psci_plat_pm_ops->pwr_domain_suspend &&
			psci_plat_pm_ops->pwr_domain_suspend_finish)
		psci_caps |=  define_psci_cap(PSCI_CPU_SUSPEND_AARCH64);
	if (psci_plat_pm_ops->system_off)
		psci_caps |=  define_psci_cap(PSCI_SYSTEM_OFF);
	if (psci_plat_pm_ops->system_reset)
		psci_caps |=  define_psci_cap(PSCI_SYSTEM_RESET);

	return 0;
}
