/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/sched.h>

#include <pf_q-group.h>
#include <pf_q-devmap.h>
#include <pf_q-bitops.h>
#include <pf_q-engine.h>


DEFINE_SEMAPHORE(group_sem);


static struct pfq_group pfq_groups[Q_MAX_GROUP];


bool
__pfq_group_access(int gid, int id, int policy, bool create)
{
        struct pfq_group * g = pfq_get_group(gid);

        if (!g)
                return false;

        switch(g->policy)
        {
        case Q_POLICY_GROUP_PRIVATE:
                return __pfq_has_joined_group(gid,id);

        case Q_POLICY_GROUP_RESTRICTED:
                return (create == false || policy == Q_POLICY_GROUP_RESTRICTED) && g->pid == current->tgid;

        case Q_POLICY_GROUP_SHARED:
                return create == false || policy == Q_POLICY_GROUP_SHARED;

        case Q_POLICY_GROUP_UNDEFINED:
                return true;
        }

        return false;
}


static void
__pfq_group_init(int gid)
{
        struct pfq_group * g = pfq_get_group(gid);
        int i;

        if (!g)
                return;

	g->pid = current->tgid;
        g->owner = -1;
        g->policy = Q_POLICY_GROUP_UNDEFINED;

        for(i = 0; i < Q_CLASS_MAX; i++)
        {
                atomic_long_set(&g->sock_mask[i], 0);
        }

        atomic_long_set(&g->bp_filter,0L);
        atomic_long_set(&g->comp,     0L);
        atomic_long_set(&g->comp_ctx, 0L);

	pfq_group_stats_reset(&g->stats);

        for(i = 0; i < Q_MAX_COUNTERS; i++)
        {
                sparse_set(&g->context.counter[i], 0);
        }

	for(i = 0; i < Q_MAX_PERSISTENT; i++)
	{
		spin_lock_init(&g->context.persistent[i].lock);
		memset(g->context.persistent[i].memory, 0, sizeof(g->context.persistent[i].memory));
	}
}


static void
__pfq_group_free(int gid)
{
        struct pfq_group * g = pfq_get_group(gid);
        struct sk_filter *filter;
        struct pfq_computation_tree *old_comp;
        void *old_ctx;

        if (!g)
                return;

        /* remove this gid from demux matrix */

        pfq_devmap_update(map_reset, Q_ANY_DEVICE, Q_ANY_QUEUE, gid);

        g->pid = 0;
        g->owner = -1;
        g->policy = Q_POLICY_GROUP_UNDEFINED;

        filter   = (struct sk_filter *)atomic_long_xchg(&g->bp_filter, 0L);
        old_comp = (struct pfq_computation_tree *)atomic_long_xchg(&g->comp, 0L);
        old_ctx  = (void *)atomic_long_xchg(&g->comp_ctx, 0L);

        msleep(Q_GRACE_PERIOD);   /* sleeping is possible here: user-context */

	/* call fini on old computation */

	if (old_comp)
 		pfq_computation_fini(old_comp);

        kfree(old_comp);
        kfree(old_ctx);

	if (filter)
        	pfq_free_sk_filter(filter);

        g->vlan_filt = false;
        pr_devel("[PFQ] group %d destroyed.\n", gid);
}


static int
__pfq_join_group(int gid, int id, unsigned long class_mask, int policy)
{
        struct pfq_group * g = pfq_get_group(gid);
        unsigned long tmp = 0;
        unsigned long bit;

        if (!g)
                return -EINVAL;

	/* if this group is unused, initializes it */

        if (!g->pid)
                __pfq_group_init(gid);

        if (!__pfq_group_access(gid, id, policy, true)) {
                pr_devel("[PFQ] gid=%d is not joinable with policy %d\n", gid, policy);
                return -EPERM;
        }

        pfq_bitwise_foreach(class_mask, bit,
        {
                 int class = pfq_ctz(bit);
                 tmp = atomic_long_read(&g->sock_mask[class]);
                 tmp |= 1L << id;
                 atomic_long_set(&g->sock_mask[class], tmp);
        })

	if (g->owner == -1) {
		g->owner = id;
	}

	if (g->policy == Q_POLICY_GROUP_UNDEFINED) {
		g->policy = policy;
	}

        return 0;
}


static int
__pfq_leave_group(int gid, int id)
{
        struct pfq_group * g = pfq_get_group(gid);
        unsigned long tmp;
        int i;

        if (!g)
                return -EINVAL;

	if (!g->pid)
		return -EPERM;

        for(i = 0; i < Q_CLASS_MAX; ++i)
        {
                tmp = atomic_long_read(&g->sock_mask[i]);
                tmp &= ~(1L << id);
                atomic_long_set(&g->sock_mask[i], tmp);
        }

        if (__pfq_group_is_empty(gid))
                __pfq_group_free(gid);

        return 0;
}

unsigned long
__pfq_get_all_groups_mask(int gid)
{
        struct pfq_group * g = pfq_get_group(gid);
        unsigned long mask = 0;
        int i;

        if (!g)
                return mask;

        for(i = 0; i < Q_CLASS_MAX; ++i)
        {
                mask |= atomic_long_read(&g->sock_mask[i]);
        }
        return mask;
}


void __pfq_set_group_filter(int gid, struct sk_filter *filter)
{
        struct pfq_group * g = pfq_get_group(gid);
        struct sk_filter * old_filter;

        if (!g) {
                pfq_free_sk_filter(filter);
                return;
        }

        old_filter = (void *)atomic_long_xchg(&g->bp_filter, (long)filter);

        msleep(Q_GRACE_PERIOD);

	if (old_filter)
        	pfq_free_sk_filter(old_filter);
}


void __pfq_dismiss_function(void *f)
{
        int n;

        for(n = 0; n < Q_MAX_GROUP; n++)
        {
                struct pfq_computation_tree *comp = (struct pfq_computation_tree *)atomic_long_read(&pfq_get_group(n)->comp);

                BUG_ON(comp != NULL);
        }

        printk(KERN_INFO "[PFQ] function @%p dismissed.\n", f);
}


int pfq_set_group_prog(int gid, struct pfq_computation_tree *comp, void *ctx)
{
        struct pfq_group * g = pfq_get_group(gid);
        struct pfq_computation_tree *old_comp;
        void *old_ctx;

        if (!g)
                return -EINVAL;

        down(&group_sem);

        old_comp = (struct pfq_computation_tree *)atomic_long_xchg(&g->comp, (long)comp);
        old_ctx  = (void *)atomic_long_xchg(&g->comp_ctx, (long)ctx);

        msleep(Q_GRACE_PERIOD);   /* sleeping is possible here: user-context */

	/* call fini on old computation */

	if (old_comp)
 		pfq_computation_fini(old_comp);

        /* free the old computation/context */

        kfree(old_comp);
        kfree(old_ctx);

        up(&group_sem);
        return 0;
}


int
pfq_join_group(int gid, int id, unsigned long class_mask, int policy)
{
        struct pfq_group * g = pfq_get_group(gid);
        int ret;

        if (!g)
                return -EINVAL;

        down(&group_sem);

        ret = __pfq_join_group(gid, id, class_mask, policy);

        up(&group_sem);
        return ret;
}


int
pfq_join_free_group(int id, unsigned long class_mask, int policy)
{
        int n = 0;

        down(&group_sem);
        for(; n < Q_MAX_ID; n++)
        {
                if(!pfq_get_group(n)->pid) {
                        __pfq_join_group(n, id, class_mask, policy);
                        up(&group_sem);
                        return n;
                }
        }
        up(&group_sem);
        return -EPERM;
}


int
pfq_leave_group(int gid, int id)
{
        struct pfq_group * g = pfq_get_group(gid);
        int ret;

        if (!g)
                return -EINVAL;

        down(&group_sem);
        ret = __pfq_leave_group(gid,id);
        up(&group_sem);
        return ret;
}


void
pfq_leave_all_groups(int id)
{
        int n = 0;
        down(&group_sem);
        for(; n < Q_MAX_ID; n++)
        {
                __pfq_leave_group(n, id);
        }
        up(&group_sem);
}


unsigned long
pfq_get_groups(int id)
{
        unsigned long ret = 0;
        int n = 0;
        down(&group_sem);
        for(; n < Q_MAX_ID; n++)
        {
                unsigned long mask = __pfq_get_all_groups_mask(n);
                if(mask & (1L << id))
                        ret |= (1UL << n);
        }
        up(&group_sem);
        return ret;
}


struct pfq_group *
pfq_get_group(int gid)
{
        if (gid < 0 || gid >= Q_MAX_GROUP) {
                pr_devel("[PFQ] get_group error: invalid group id %d!\n", gid);
                return NULL;
	}

        return &pfq_groups[gid];
}


bool __pfq_vlan_filters_enabled(int gid)
{
        struct pfq_group *g = pfq_get_group(gid);
        if (!g)
        	return false;
        return g->vlan_filt;
}


bool __pfq_check_group_vlan_filter(int gid, int vid)
{
        struct pfq_group *g = pfq_get_group(gid);
        if (!g)
                return false;

        return g->vid_filters[vid & 4095];
}


bool __pfq_toggle_group_vlan_filters(int gid, bool value)
{
        struct pfq_group *g = pfq_get_group(gid);
        if (!g)
                return false;

        if (value)
                memset(g->vid_filters, 0, 4096);

        smp_wmb();

        g->vlan_filt = value;
        return true;
}


void __pfq_set_group_vlan_filter(int gid, bool value, int vid)
{
        struct pfq_group *g = pfq_get_group(gid);
        if (!g)
                return;

        g->vid_filters[vid & 4095] = value;
}



int pfq_check_group(int id, int gid, const char *msg)
{
        if (gid < 0 || gid >= Q_MAX_GROUP) {
                printk(KERN_INFO "[PFQ|%d] %s error: invalid group (gid=%d)!\n", id, msg, gid);
                return -EINVAL;
        }
        return 0;
}


int pfq_check_group_access(int id, int gid, const char *msg)
{
	struct pfq_group *g;
	int err;

	err = pfq_check_group(id, gid, msg);
	if (err != 0)
		return err;

        if (!__pfq_has_joined_group(gid, id)) {
                printk(KERN_INFO "[PFQ|%d] %s error: permission denied (gid=%d)!\n", id, msg, gid);
                return -EPERM;
        }

	g = pfq_get_group(gid);
	if (g == NULL || g->owner != id) {
                printk(KERN_INFO "[PFQ|%d] %s: invalid owner (id=%d)!\n", id, msg, g->owner);
                return -EACCES;
	}
	return 0;
}
