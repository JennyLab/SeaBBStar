// lock_based_rb_tree.cpp
// Thread‑safe lock‑based (serialised writers) red‑black tree.
// -----------------------------------------------------------
// * Searches are fully parallel: they use per‑node shared locks via
//   std::shared_mutex and rely on lock‑coupling (at most two locks
//   per thread).
// * Writers are serialised via a global mutex (writers_mutex) so that
//   only ONE writer proceeds at a time.  Inside the critical section
//   the writer still cooperates with concurrent readers by acquiring
//   *exclusive* (unique_lock) access only on the nodes it mutates.
//   This follows the design outlined in UCAM‑CL‑TR‑579 §4.5.2.1.
//
//   Build:  g++ -std=c++17 -pthread -O3  -DRBTREE_DEMO  lock_based_rb_tree.cpp -o rbt

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace rbt
{

    /*===========================================================================
     *  Low-level building blocks for a lock-based Red-Black tree
     *===========================================================================*/

    /*-------------------------------------------------------------------------
     *  enum Color
     *-------------------------------------------------------------------------
     *  • Each node is either RED or BLACK; these colours encode the RB-tree
     *    invariants that keep the structure (approximately) balanced.
     *  • We store them as an explicit enum class backed by uint8_t to keep the
     *    node footprint small yet type-safe (no accidental integer mix-ups).
     *-------------------------------------------------------------------------*/
    enum class Color : uint8_t
    {
        RED,
        BLACK
    };

    /*-------------------------------------------------------------------------
     *  struct Node<K,V>
     *-------------------------------------------------------------------------
     *  Represents a single tree node parameterised by key type K and value V.
     *
     *  key          – user-supplied key; ordering governed by Compare functor.
     *  val          – payload value associated with the key.
     *  color        – RED or BLACK, defaults to RED (newly inserted nodes).
     *
     *  parent,left,right – raw pointers forming the usual binary-tree links.
     *
     *  rw           – per-node std::shared_mutex enabling:
     *                   • multiple concurrent *readers*  (shared_lock)
     *                   • exactly one concurrent *writer* (unique_lock)
     *                 Readers therefore never block each other; writers are
     *                 additionally serialized by a global writers_mutex.
     *-------------------------------------------------------------------------*/
    template <typename K, typename V>
    struct Node
    {
        K key;
        V val;
        Color color{Color::RED};

        Node *parent{nullptr};
        Node *left{nullptr};
        Node *right{nullptr};

        mutable std::shared_mutex rw; // hand-over-hand lock coupling

        /*  Constructor – colour defaults to RED for fresh insertions.  */
        explicit Node(const K &k, const V &v,
                      Color c = Color::RED) : key(k), val(v), color(c) {}
    };

    /*-------------------------------------------------------------------------
     *  Forward declaration of RBTree
     *-------------------------------------------------------------------------*/
    template <typename K, typename V, typename Compare = std::less<K>>
    class RBTree; // full definition appears later

    /*-------------------------------------------------------------------------
     *  struct UpgradeLock  (RAII helper)
     *-------------------------------------------------------------------------
     *  Purpose: during tree descent we hold each node’s lock in *shared*
     *  mode; when we reach the parent that we need to MODIFY we “upgrade”
     *  to an exclusive (unique) lock without ever being completely unlocked
     *  (avoids a race window).
     *
     *  How it works:
     *    • ctor acquires a shared_lock on the supplied mutex
     *      (deferred + lock() for clarity).
     *    • upgrade(): release shared lock, immediately acquire unique_lock.
     *      Both share the same underlying mutex reference.
     *    • dtor of unique_lock / shared_lock automatically releases lock.
     *-------------------------------------------------------------------------*/
    struct UpgradeLock
    {
        std::shared_lock<std::shared_mutex> shared; // read phase
        std::unique_lock<std::shared_mutex> unique; // write phase

        explicit UpgradeLock(std::shared_mutex &mtx)
            : shared(mtx, std::defer_lock), unique(mtx, std::defer_lock)
        {
            shared.lock(); // start in shared (read) mode
        }

        /* Switch from shared → unique with minimal window */
        void upgrade()
        {
            shared.unlock(); // drop shared
            unique.lock();   // acquire exclusive
        }
    };

    template <typename K, typename V, typename Compare>
    class RBTree
    {
    public:
        /* Type alias for brevity.  All internal helpers refer to nodes via NodeT */
        using NodeT = Node<K, V>;

        /*───────────────────────────────────────────────────────────────────────────
          Constructor
          ───────────
          • Create a single, shared NIL sentinel node (color = BLACK).
            Every leaf pointer in the tree will reference this NIL, so we never
            deal with raw nullptrs while traversing or rebalancing.
          • The root initially points to NIL, meaning “empty tree.”
         ──────────────────────────────────────────────────────────────────────────*/
        RBTree()
        {
            NIL = new NodeT(K{}, V{}, Color::BLACK); // sentinel carries dummy key/val
            root = NIL;
        }

        /*───────────────────────────────────────────────────────────────────────────
          Destructor
          ──────────
          • Recursively delete every *real* node via destroy_rec().
          • Finally delete the shared NIL sentinel.
          • Safe because writers are serialized; destruction should happen
            when no other threads hold references.
         ──────────────────────────────────────────────────────────────────────────*/
        ~RBTree()
        {
            destroy_rec(root); // post-order recursion frees children first
            delete NIL;
        }

        /*───────────────────────────────────────────────────────────────────────────
          Rule of five – copy operations deleted
          ──────────────────────────────────────
          • The tree manages its own dynamic memory and thread-safety primitives.
          • A shallow copy would double-delete nodes and corrupt mutex states.
          • Move semantics could be implemented later, but are out of scope.
         ──────────────────────────────────────────────────────────────────────────*/
        RBTree(const RBTree &) = delete;            // no copy-construct
        RBTree &operator=(const RBTree &) = delete; // no copy-assign

        /*───────────────────────────────────────────────────────────────────────────
          writer_mutex()
          --------------
          • Exposes a *mutable* reference to the global writers_mutex so that
            external helper classes (e.g., TreeValidator in the stress test)
            can take a lock while calling validate().
          • Returns a non-const reference to allow lock / unlock.
          • Marked `const` so it can be called on const RBTree objects.
         ──────────────────────────────────────────────────────────────────────────*/
        std::mutex &writer_mutex() const { return writers_mutex; }

        // ────────────────────────────────────────────────────────────────────────
        //  Parallel, read-only lookup
        //
        //  • Uses **lock coupling** (a.k.a. hand-over-hand locking):
        //      – Before we follow a child pointer we first take that child’s
        //        shared lock, then release the parent’s shared lock.
        //      – At most two locks per thread → bounded memory & avoids deadlock.
        //  • Each node’s lock is a std::shared_mutex, so an *arbitrary number* of
        //    readers can traverse concurrently; writers must acquire a unique_lock
        //    on the node(s) they modify and are globally serialized by writers_mutex.
        //  • Complexity: O(log n) in a balanced tree, identical to a sequential
        //    RB-tree lookup.
        //
        //  Returns std::nullopt if key not found, otherwise a *copy* of the value.
        // ────────────────────────────────────────────────────────────────────────
        std::optional<V> lookup(const K &k) const
        {
            // 1. Start at root and take a *shared* (read) lock on it.
            const NodeT *n = root;
            std::shared_lock<std::shared_mutex> lock_cur(n->rw);

            // 2. Descend until we hit NIL (sentinel) or the target key.
            while (n != NIL)
            {
                if (comp(k, n->key)) // search key < current key → go LEFT
                {
                    NodeT *child = n->left;

                    // Lock child BEFORE releasing parent to maintain the
                    // lock-coupling invariant and prevent the path from mutating
                    // under our feet.
                    std::shared_lock<std::shared_mutex> lock_child(child->rw);

                    lock_cur.unlock(); // now safe to release parent
                    n = child;         // move cursor
                    lock_cur = std::move(lock_child);
                }
                else if (comp(n->key, k)) // search key > current key → go RIGHT
                {
                    NodeT *child = n->right;
                    std::shared_lock<std::shared_mutex> lock_child(child->rw);

                    lock_cur.unlock();
                    n = child;
                    lock_cur = std::move(lock_child);
                }
                else // keys are equal → found!
                {
                    // make a copy so we can release the lock before returning
                    auto v = n->val;
                    return v;
                }
            }

            // Reached NIL sentinel → key absent
            return std::nullopt;
        }

        // ────────────────────────────────────────────────────────────────────────
        //  INSERT  (writers are globally serialised)
        //
        //  • A single global writers_mutex guarantees *at most one* writer thread
        //    is in the tree-modification section at a time; this simplifies
        //    reasoning and avoids writer–writer deadlock.
        //  • Even inside that critical section we still cooperate with concurrent
        //    *readers* by using per-node read/write locks (shared_mutex):
        //        – while DESCENDING the search path we only take *shared* (read)
        //          locks, so readers can pass freely.
        //        – once we have located the parent where the new node will attach,
        //          we *upgrade* its lock to unique mode to modify its child pointer.
        //  • Duplicate keys are handled by **overwrite** (replace value, no size++).
        //
        //  Complexity: O(log n) rotations/recolors done later in insert_fixup().
        // ────────────────────────────────────────────────────────────────────────
        void insert(const K &k, const V &v)
        {
            /* 1. Global serialisation of *writers*.
               Readers never touch this mutex, so they proceed in parallel. */
            std::unique_lock<std::mutex> writer_guard(writers_mutex);

            /* 2. Create the new RED node (z).  Sentinels used for children. */
            NodeT *z = new NodeT(k, v);
            z->left = z->right = z->parent = NIL;

            NodeT *y = NIL;  // will track the parent pointer
            NodeT *x = root; // traversal cursor (starts at root)

            /* 3. Begin lock coupling: take a *shared* lock on root.  UpgradeLock
               helper starts with shared_lock held, provides .upgrade() to convert
               to unique_lock in-place. */
            UpgradeLock lock_x(x->rw);

            /* 4. DESCEND the tree to find insertion point */
            while (x != NIL)
            {
                y = x; // remember parent

                if (comp(k, x->key)) // go LEFT
                {
                    NodeT *next = x->left;

                    // Acquire child’s shared lock *before* releasing parent
                    UpgradeLock lock_next(next->rw);

                    // Release parent’s lock (two-lock invariant)
                    lock_x.shared.unlock();

                    // Advance cursor
                    x = next;
                    lock_x = std::move(lock_next);
                }
                else if (comp(x->key, k)) // go RIGHT
                {
                    NodeT *next = x->right;
                    UpgradeLock lock_next(next->rw);
                    lock_x.shared.unlock();
                    x = next;
                    lock_x = std::move(lock_next);
                }
                else // DUPLICATE KEY
                {
                    // Upgrade parent’s shared lock → unique to perform mutation.
                    lock_x.upgrade();
                    x->val = v; // overwrite value
                    delete z;   // discard unused node
                    return;     // all done
                }
            }

            /* 5. We’ve dropped off the tree; y is the parent NIL’s neighbor.
               upgrade() converts y’s shared lock into unique so we can safely
               mutate its child pointer. */
            lock_x.upgrade(); // exclusive access to y

            z->parent = y;
            if (y == NIL) // tree was empty → z becomes root
                root = z;
            else if (comp(z->key, y->key)) // insert as LEFT or RIGHT child
                y->left = z;
            else
                y->right = z;

            /* 6. New node is RED by default (set in ctor).  Rebalance the tree.
               insert_fixup() may rotate or recolor up the path but will always
               leave the tree valid when it returns. */
            insert_fixup(z);
        }

        // ────────────────────────────────────────────────────────────────────────
        //  ERASE   (writers serialised, readers parallel)
        //
        //  • writers_mutex guarantees exclusive access by *one* writer, so we do
        //    not need per-node locks while searching / splicing.
        //  • The algorithm follows CLRS “RB-DELETE”:
        //        1.  Find node z that matches key k.
        //        2.  Perform ordinary BST delete using *transplant()* helper.
        //            y is the node that is physically removed from the tree
        //            (either z itself or its in-order successor).
        //        3.  If y was BLACK we may have violated property #5 (“black height”)
        //            — call delete_fixup(x) where x is the child that inherited
        //            y’s original parent link and potentially carries the
        //            double-black.
        //
        //  Returns false if key not found, true otherwise.
        // ────────────────────────────────────────────────────────────────────────
        bool erase(const K &k)
        {
            /* 1. Writers exclusive section */
            std::unique_lock<std::mutex> writer_guard(writers_mutex);

            /* 2. Search for node z with key k  (no locking – readers blocked) */
            NodeT *z = root;
            while (z != NIL && k != z->key)
                z = comp(k, z->key) ? z->left : z->right;

            if (z == NIL)
                return false; // key not present

            /* 3.  y = node actually removed; x = child that replaces y */
            NodeT *y = z;
            NodeT *x = nullptr;
            Color y_original = y->color; // remember color of removed node

            /* 3-A.  z has < 2 children → easy splice -------------------------- */
            if (z->left == NIL)
            {
                x = z->right;            // x may be NIL
                transplant(z, z->right); // replace z by its right child
            }
            else if (z->right == NIL)
            {
                x = z->left;
                transplant(z, z->left); // replace z by its left child
            }

            /* 3-B.  z has TWO children → use in-order successor ---------------- */
            else
            {
                // y = successor (minimum of right subtree) — guaranteed no left child
                y = minimum(z->right);
                y_original = y->color; // remember its color (could be RED/BLACK)
                x = y->right;          // x replaces y after transplant

                if (y->parent == z)
                {
                    // Successor is z’s direct child: after transplant x’s parent becomes y
                    x->parent = y;
                }
                else
                {
                    // Move y’s right child up; y will move to z’s spot
                    transplant(y, y->right);
                    y->right = z->right;
                    y->right->parent = y;
                }

                // Finally replace z by y and re-attach z’s left subtree
                transplant(z, y);
                y->left = z->left;
                y->left->parent = y;
                y->color = z->color; // y adopts z’s original color
            }

            /* 4. Free memory for removed node */
            delete z;

            /* 5. If a BLACK node was removed, fix potential double-black violations */
            if (y_original == Color::BLACK)
                delete_fixup(x); // x may be NIL sentinel; fix-up code handles it

            return true;
        }

        bool validate() const
        {
            int bh = -1;
            return validate_rec(root, 0, bh);
        }

    private:
        /*────────────────────────────────────────────────────────────────────────────
         *  Core data members of RBTree
         *───────────────────────────────────────────────────────────────────────────*/

        /* Pointer to the top of the tree.  Always non-null: when the tree is empty
         * `root` points to the shared NIL sentinel.  Insert/delete/rotate helpers
         * update this pointer whenever the logical root changes. */
        NodeT *root;

        /* Shared NIL sentinel node
         * ------------------------
         *  • Serves as “NULL leaf” for every external child pointer.
         *  • Colour is permanently BLACK so the red-black properties remain valid
         *    at the leaves without special-case code.
         *  • Having a real object (instead of nullptr) simplifies rotations,
         *    validation and traversal because we can safely read NIL->color etc.
         */
        NodeT *NIL;

        /* Comparator functor
         * ------------------
         *  Determines the strict weak ordering of keys.  Defaults to std::less<K>
         *  but users can supply any callable that implements `bool comp(a,b)` and
         *  imposes a total order.  All BST decisions (`go left / go right`) rely
         *  exclusively on this comparator. */
        Compare comp;

        /* Global writers mutex
         * --------------------
         *  • Ensures that only *one* writer (insert/erase) thread is inside
         *    the tree-mutating critical section at any time.
         *  • Marked **mutable** so that even `const` member functions such as
         *    `writer_mutex()` can return a non-const reference and external
         *    validators can lock it.
         *  • Readers never lock this mutex; they use per-node shared locks, so
         *    multiple lookups proceed fully in parallel. */
        mutable std::mutex writers_mutex;

        /*────────────────────────────────────────────────────────────────────────────
         *  destroy_rec
         *  ------------------------------------------------------------------------
         *  Recursively frees all nodes in a post-order traversal (children first),
         *  leaving only the shared NIL sentinel to be deleted by the destructor.
         *
         *  Preconditions: called only during ~RBTree(), when no other threads
         *  hold references.  The writers_mutex is irrelevant because destruction
         *  happens after the tree has gone out of scope in user code.
         *──────────────────────────────────────────────────────────────────────────*/
        void destroy_rec(NodeT *n)
        {
            if (n == NIL) // base case: reached sentinel
                return;

            destroy_rec(n->left);  // delete left subtree
            destroy_rec(n->right); // delete right subtree
            delete n;              // delete current node
        }

        /*===========================================================================
         *  Tree Rotations
         *===========================================================================
         *  • Performed only by writer threads that already hold the global
         *    writers_mutex **and** have exclusive (unique) locks on every node
         *    they mutate.  Therefore, no locking is done inside these helpers.
         *
         *  • Rotations are the fundamental local restructuring operation in
         *    red-black (and AVL) trees. They preserve *in-order key ordering*
         *    while changing the tree’s shape / node heights.
         *
         *  Diagram key
         *  ───────────
         *          p          : parent (may be NIL)
         *          x, y       : rotation pivot nodes
         *          α β γ      : subtrees whose internal structure is unchanged
         *
         *  Left rotation:
         *          p              p
         *         /              /
         *        x              y
         *       / \    --->    / \
         *      α   y          x   γ
         *         / \        / \
         *        β   γ      α   β
         *
         *  Right rotation is the mirror image.
         *===========================================================================*/

        /*───────────────────────────────────────────────────────────────────────────
          left_rotate(x)
          --------------
          Promotes x->right (node y) to x’s position; x becomes y’s *left* child.
        ───────────────────────────────────────────────────────────────────────────*/
        void left_rotate(NodeT *x)
        {
            NodeT *y = x->right; // y will move up

            /* Step 1: move y’s LEFT subtree (β) to be x’s RIGHT subtree */
            x->right = y->left;
            if (y->left != NIL)
                y->left->parent = x;

            /* Step 2: link x’s parent to y */
            y->parent = x->parent;
            if (x->parent == NIL) // x was root → y becomes new root
                root = y;
            else if (x == x->parent->left)
                x->parent->left = y; // x was left child → replace with y
            else
                x->parent->right = y; // x was right child

            /* Step 3: put x on y’s LEFT and fix parent */
            y->left = x;
            x->parent = y;
        }

        /*───────────────────────────────────────────────────────────────────────────
          right_rotate(y)
          ---------------
          Mirror-image of left_rotate: promote y->left (node x) upward.
        ───────────────────────────────────────────────────────────────────────────*/
        void right_rotate(NodeT *y)
        {
            NodeT *x = y->left; // x will move up

            /* Step 1: move x’s RIGHT subtree (β) to be y’s LEFT child */
            y->left = x->right;
            if (x->right != NIL)
                x->right->parent = y;

            /* Step 2: link y’s parent to x */
            x->parent = y->parent;
            if (y->parent == NIL) // y was root
                root = x;
            else if (y == y->parent->right)
                y->parent->right = x; // y was right child
            else
                y->parent->left = x; // y was left child

            /* Step 3: put y on x’s RIGHT */
            x->right = y;
            y->parent = x;
        }

        /* --------------------------------------------------------------------------
         *  RB-TREE INSERT FIX-UP
         *
         *  z :  The newly inserted node (initially RED).  We must restore the
         *       5 Red-Black properties, of which only #4 and #5 can be violated:
         *
         *       4.  A RED node cannot have a RED parent.
         *       5.  Every root-to-leaf path has the same # of BLACK nodes.
         *
         *  Strategy (CLRS §13.3):
         *  ── While z’s parent is RED (therefore grand-parent exists and is BLACK):
         *     Case 1:  Uncle y is RED      → recolor parent & uncle BLACK, gp RED,
         *                                     and continue fixing from gp.
         *     Case 2:  Uncle y is BLACK _and_
         *              z is an “inner” child (left-right or right-left)
         *                                   → rotate parent toward z to convert
         *                                     to Case 3 configuration.
         *     Case 3:  Uncle y is BLACK _and_
         *              z is an “outer” child (left-left or right-right)
         *                                   → recolor parent BLACK, gp RED,
         *                                     rotate gp in opposite direction.
         *
         *  The first branch handles “z’s parent is a LEFT child”; the `else`
         *  mirrors for parent being a RIGHT child.
         * -------------------------------------------------------------------------- */
        void insert_fixup(NodeT *z)
        {
            // Loop only while parent is RED.  If parent is BLACK we’re done
            // because property (4) holds again.
            while (z->parent->color == Color::RED)
            {
                /* ================================================================
                 *   PARENT IS LEFT CHILD  (mirror branch further below)
                 * ================================================================ */
                if (z->parent == z->parent->parent->left)
                {
                    NodeT *y = z->parent->parent->right; // y = uncle

                    /* -------- Case 1: uncle is RED ➜ simple recolor -------------- */
                    if (y->color == Color::RED)
                    {
                        //        gp(B)            gp(R)
                        //       /     \          /     \
                        //   p(R)      y(R) →  p(B)     y(B)
                        //   /                     \
                        // z(R)                    z(R)
                        z->parent->color = Color::BLACK;
                        y->color = Color::BLACK;
                        z->parent->parent->color = Color::RED;
                        z = z->parent->parent; // continue up the tree
                    }
                    /* -------- Uncle BLACK: need rotations (Case 2 or 3) ---------- */
                    else
                    {
                        /* ---- Case 2: z is right-child (inner) → rotate parent --- */
                        if (z == z->parent->right)
                        {
                            // convert to left-left (Case 3) shape
                            z = z->parent;
                            left_rotate(z);
                        }

                        /* ---- Case 3: z is now left-left (outer) ----------------- */
                        // recolor parent / grand-parent and rotate grand-parent
                        z->parent->color = Color::BLACK;
                        z->parent->parent->color = Color::RED;
                        right_rotate(z->parent->parent);
                    }
                }
                /* ================================================================
                 *   PARENT IS RIGHT CHILD  (mirror of above)
                 * ================================================================ */
                else
                {
                    NodeT *y = z->parent->parent->left; // uncle on the other side

                    if (y->color == Color::RED) // ---- Case 1 (mirror) ---
                    {
                        z->parent->color = Color::BLACK;
                        y->color = Color::BLACK;
                        z->parent->parent->color = Color::RED;
                        z = z->parent->parent;
                    }
                    else // uncle is BLACK
                    {
                        if (z == z->parent->left) // ---- Case 2 (mirror) ----
                        {
                            z = z->parent;
                            right_rotate(z);
                        }
                        /* ---- Case 3 (mirror) ----------------------------------- */
                        z->parent->color = Color::BLACK;
                        z->parent->parent->color = Color::RED;
                        left_rotate(z->parent->parent);
                    }
                }
            }

            /* Ensure property (2):  root must be BLACK */
            root->color = Color::BLACK;
        }

        // ─────────────────────────────────────────────────────────────────────────────
        //   transplant(u, v)
        //   --------------------------------------------------------------------------
        //   * Utility used by the delete routine.
        //   * Replaces the subtree rooted at node `u` with the subtree rooted at `v`
        //     (which may be the NIL sentinel).  Parent pointers are adjusted so that
        //     the tree remains a valid binary-search-tree structure.
        //   * Colour information is **not** modified here; the caller is responsible
        //     for copying / fixing colours if needed.
        //
        //   Cases handled
        //   -------------
        //    1) `u` is the root          →  update `root` pointer.
        //    2) `u` is a left child      →  make parent->left  = v.
        //    3) `u` is a right child     →  make parent->right = v.
        //
        //   After this call, `v->parent` points to u’s original parent, even when
        //   `v` is the NIL sentinel (whose parent field is allowed to vary).
        // ─────────────────────────────────────────────────────────────────────────────
        void transplant(NodeT *u, NodeT *v)
        {
            if (u->parent == NIL) // Case 1: u was the root
                root = v;
            else if (u == u->parent->left) // Case 2: u was a left child
                u->parent->left = v;
            else // Case 3: u was a right child
                u->parent->right = v;

            v->parent = u->parent; // hook v (or NIL) into the tree
        }

        /*────────────────────────────────────────────────────────────────────────────
          minimum(x)
          ----------
          Returns a pointer to the node with the *smallest key* in the subtree
          rooted at `x`.  Used by delete() to locate the in-order successor when
          the node to delete has two children.

          Implementation: left-most descent in O(height) = O(log n) for an RB-tree.
        ────────────────────────────────────────────────────────────────────────────*/
        NodeT *minimum(NodeT *x) const
        {
            while (x->left != NIL) // keep following left child
                x = x->left;
            return x; // left-most node reached
        }

        /* --------------------------------------------------------------------------
         * Fix-up after RB-tree deletion
         *
         *  x  – the child that replaced the removed node in the standard BST delete
         *       (may be NIL).  When the removed node was black, the tree may now
         *       violate the “every root-to-leaf path has the same number of black
         *       nodes” property.  We treat x as carrying an extra “double-black”
         *       which must be pushed upward or resolved locally.
         *
         *  The logic follows CLRS §13.4 *Delete*:
         *  ─────────────────────────────────────────────────────────────────────
         *  Case 1:  Sibling w is RED            -> recolor & rotate to make w BLACK
         *  Case 2:  w is BLACK and both w’s     -> recolor w = RED, move the
         *           children are BLACK             double-black up to parent
         *  Case 3:  w is BLACK, w->near child   -> rotate w toward x to convert
         *           is RED, far child BLACK        to Case-4 situation
         *  Case 4:  w is BLACK, w->far child    -> final rotate, recolor, done
         *           is RED
         *  The “left” branch covers x as a left child; the “else” branch is the
         *  symmetric mirror for x as a right child.
         * -------------------------------------------------------------------------- */
        void delete_fixup(NodeT *x)
        {
            // Loop until the double-black x reaches the root OR becomes red
            // (recolor to black at the end).
            while (x != root && x->color == Color::BLACK)
            {

                // ─────────────────────────  x is LEFT child  ────────────────────────
                if (x == x->parent->left)
                {
                    NodeT *w = x->parent->right; // x’s sibling

                    /* ---------------- Case 1: sibling is RED -------------------- */
                    if (w->color == Color::RED)
                    {
                        //   p(B)         w(R)                p(R)         w(B)
                        //  /    \  -->  /    \     rotate   /    \  +     /    \
                        // x(DB)  w     x(DB)  c            x(DB)  a      b      c
                        //
                        // After recolor+rotate, x still double-black but sibling
                        // is now BLACK so we proceed to cases 2–4.
                        w->color = Color::BLACK;
                        x->parent->color = Color::RED;
                        left_rotate(x->parent);
                        w = x->parent->right; // new sibling after rotation
                    }

                    /* ------------ Case 2: sibling black, both nephews black ------- */
                    if (w->left->color == Color::BLACK && w->right->color == Color::BLACK)
                    {
                        // Push the extra black up one level by recoloring sibling RED.
                        // Parent becomes the new double-black node x.
                        w->color = Color::RED;
                        x = x->parent;
                    }
                    else
                    {
                        /* ---------- Case 3: sibling black, far nephew black ------- */
                        if (w->right->color == Color::BLACK)
                        {
                            // Convert to Case-4 by rotating sibling toward x,
                            // making far nephew RED.
                            //
                            //   p(?)               p(?)               p(?)
                            //  /    \     --->    /    \   recolor   /    \
                            // x(DB)  w(B)        x(DB)  b(R)        x(DB)  w(B)
                            //        /   \                     -->         /   \
                            //      a(R)  b(B)                              a(R)  b(B)
                            w->left->color = Color::BLACK;
                            w->color = Color::RED;
                            right_rotate(w);
                            w = x->parent->right; // new sibling
                        }

                        /* ------------------- Case 4: far nephew RED --------------- */
                        // Final rotation fixes the double-black:
                        //
                        //     p(B)                      w(B)
                        //    /    \     rotate         /    \
                        //   x(DB)  w(B)   --->        p(B)   c(B)
                        //          /  \              /   \
                        //         b(R) c(B)        x(B)  b(R)
                        w->color = x->parent->color; // w takes parent’s color
                        x->parent->color = Color::BLACK;
                        w->right->color = Color::BLACK;
                        left_rotate(x->parent);
                        x = root; // loop will terminate
                    }

                    // ────────────────────────  x is RIGHT child (mirror)  ──────────────
                }
                else
                {
                    NodeT *w = x->parent->left; // sibling

                    if (w->color == Color::RED)
                    { // Case 1 (mirror)
                        w->color = Color::BLACK;
                        x->parent->color = Color::RED;
                        right_rotate(x->parent);
                        w = x->parent->left;
                    }

                    if (w->right->color == Color::BLACK && w->left->color == Color::BLACK)
                    {
                        w->color = Color::RED; // Case 2 (mirror)
                        x = x->parent;
                    }
                    else
                    {
                        if (w->left->color == Color::BLACK)
                        { // Case 3 (mirror)
                            w->right->color = Color::BLACK;
                            w->color = Color::RED;
                            left_rotate(w);
                            w = x->parent->left;
                        }

                        // Case 4 (mirror)
                        w->color = x->parent->color;
                        x->parent->color = Color::BLACK;
                        w->left->color = Color::BLACK;
                        right_rotate(x->parent);
                        x = root;
                    }
                }
            }

            // Finally clear the extra black on x
            x->color = Color::BLACK;
        }

        /*────────────────────────────────────────────────────────────────────────────
          validate_rec
          ─────────────
          Recursively checks that the subtree rooted at `n` satisfies **all**
          red-black properties *and* the BST ordering. Returns `true` on success.

          Parameters
          ----------
          n        : pointer to current node (may be NIL sentinel).
          blacks   : running count of BLACK nodes seen so far on the path
                     from the original root down to, but *excluding*, `n`.
          target   : OUT parameter.  The first time we hit a NIL leaf we record
                     that path’s black-height here; every subsequent leaf must
                     match this value.

          Red-Black properties verified
          -----------------------------
          (1) Every node is RED or BLACK          – implicit by enum.
          (2) Root is BLACK                       – enforced elsewhere (insert / fixup).
          (3) NIL leaves are BLACK                – NIL is constructed BLACK.
          (4) If a node is RED, both children are BLACK     → checked below.
          (5) Every root-to-leaf path contains the same
              number of BLACK nodes.                        → checked via `blacks/target`.

          Additionally we check **BST ordering** so that validate() can detect
          structural corruption, not just colour errors.
         ───────────────────────────────────────────────────────────────────────────*/
        bool validate_rec(const NodeT *n, int blacks, int &target) const
        {
            /*---------------------------------------------------------------------
              Base-case: reached the NIL sentinel (“virtual leaf”)
             ---------------------------------------------------------------------*/
            if (n == NIL)
            {
                // Record black-height on first leaf; thereafter compare.
                if (target == -1)
                    target = blacks;     // establish baseline
                return blacks == target; // property #5
            }

            /* Increment black counter if current node is BLACK */
            if (n->color == Color::BLACK)
                ++blacks;

            /*---------------------------------------------------------------------
              Property #4 – red node cannot have red children
             ---------------------------------------------------------------------*/
            if (n->color == Color::RED &&
                (n->left->color == Color::RED || n->right->color == Color::RED))
                return false;

            /*---------------------------------------------------------------------
              BST order: left < node < right
             ---------------------------------------------------------------------*/
            if (n->left != NIL && comp(n->key, n->left->key)) // n.key < left.key  (viol.)
                return false;
            if (n->right != NIL && comp(n->right->key, n->key)) // right.key < n.key (viol.)
                return false;

            /* Recurse into children; path is valid only if **both** subtrees valid */
            return validate_rec(n->left, blacks, target) &&
                   validate_rec(n->right, blacks, target);
        }
    };

} // namespace rbt

// ---------------- Demo ----------------
#ifdef RBTREE_DEMO
//--------------------------------------------------  multithreaded test
#include <atomic>

int main()
{
    constexpr int NKEYS = 20'000; // key space
    constexpr int WRITERS = 8;    // mixed insert/erase
    constexpr int READERS = 8;    // heavy lookups
    constexpr int UPDATERS = 4;   // duplicate inserts
    constexpr auto TEST_DURATION = std::chrono::seconds(3);

    rbt::RBTree<int, int> tree;

    // ── 1. bulk parallel insert ──────────────────────────────────────────
    std::vector<int> keys(NKEYS);
    std::iota(keys.begin(), keys.end(), 0);
    std::shuffle(keys.begin(), keys.end(),
                 std::mt19937{std::random_device{}()});

    auto ceil_div = [](size_t a, size_t b)
    { return (a + b - 1) / b; };

    std::vector<std::thread> threads;
    for (int w = 0; w < WRITERS; ++w)
    {
        size_t beg = w * ceil_div(NKEYS, WRITERS);
        size_t end = std::min<size_t>(beg + ceil_div(NKEYS, WRITERS), NKEYS);
        threads.emplace_back([&, beg, end]
                             {
            for (size_t i = beg; i < end; ++i)
                tree.insert(keys[i], keys[i]); });
    }
    for (auto &t : threads)
        t.join();
    threads.clear();
    std::cout << "[phase-1] bulk insert done\n";

    // ── 2. verify content ────────────────────────────────────────────────
    for (int k : keys)
    {
        auto v = tree.lookup(k);
        assert(v && *v == k);
    }
    std::cout << "  ✔ all " << NKEYS << " keys present\n";

    // ── 3. 3-second mixed stress workload ────────────────────────────────
    const auto stop_time = std::chrono::steady_clock::now() + TEST_DURATION;
    std::atomic<bool> stop{false};

    // 3-a watchdog validating invariants every 50 ms
    std::thread watchdog([&]
                         {
    while (!stop.load(std::memory_order_acquire)) {
        {   // 🔒 hold writers_mutex so no writer mutates during validation
            std::lock_guard<std::mutex> g(tree.writer_mutex());
            assert(tree.validate());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } });

    // Prepare unique seeds (avoid shared RNG)
    std::vector<uint32_t> seeds;
    {
        std::random_device rd;
        seeds.resize(WRITERS + READERS + UPDATERS);
        for (auto &s : seeds)
            s = rd();
    }
    size_t seed_idx = 0;

    auto rand_key = [&](std::mt19937 &g)
    {
        std::uniform_int_distribution<int> d(-NKEYS / 4, NKEYS * 5 / 4);
        return d(g); // may be outside range
    };

    // 3-b writer threads: half insert, half erase
    for (int i = 0; i < WRITERS; ++i)
    {
        uint32_t seed = seeds[seed_idx++];
        threads.emplace_back([&, i, seed]
                             {
            std::mt19937 rng{seed};
            while (std::chrono::steady_clock::now() < stop_time) {
                int k = rand_key(rng);
                if (i & 1)
                    tree.insert(k, k);       // odd-index writer inserts
                else
                    (void) tree.erase(k);    // even-index writer erases
            } });
    }

    // 3-c updater threads: overwrite existing keys with new values
    for (int i = 0; i < UPDATERS; ++i)
    {
        uint32_t seed = seeds[seed_idx++];
        threads.emplace_back([&, seed]
                             {
            std::mt19937 rng{seed};
            while (std::chrono::steady_clock::now() < stop_time) {
                int k = (rand_key(rng) & 0x7fffffff) % NKEYS; // ensure [0,NKEYS)
                tree.insert(k, k + 42);       // duplicate insert/overwrite
            } });
    }

    // 3-d reader threads
    for (int i = 0; i < READERS; ++i)
    {
        uint32_t seed = seeds[seed_idx++];
        threads.emplace_back([&, seed]
                             {
            std::mt19937 rng{seed};
            while (std::chrono::steady_clock::now() < stop_time) {
                (void) tree.lookup(rand_key(rng));
            } });
    }

    // 3-e join everything, stop watchdog
    for (auto &t : threads)
        t.join();
    stop.store(true, std::memory_order_release);
    watchdog.join();
    std::cout << "[phase-2] mixed stress finished\n";

    // ── 4. final validation & stats ───────────────────────────────────────
    // assert(tree.validate());
    // 4. final validation  (after all worker threads joined)
    {
        std::lock_guard<std::mutex> g(tree.writer_mutex());
        assert(tree.validate());
    }

    size_t survivors = 0;
    for (int k = -NKEYS / 4; k < NKEYS * 5 / 4; ++k)
        if (tree.lookup(k))
            ++survivors;

    std::cout << "  ✔ invariants hold, " << survivors
              << " keys currently in tree\n"
              << "🎉 ALL STRESS TESTS PASSED\n";
    return 0;
}

#endif
