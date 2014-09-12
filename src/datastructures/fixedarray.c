#define MEM_PAGESIZE 4096

typedef struct {
	char data[MEM_PAGESIZE];
} page_frame;

typedef struct {
	page_frame ** pages;
	int size, numpages, page_cap, obj_size;
} fixed_array;

void* access(int index, fixed_array *fa) {
	return (void*)&(fa->pages[index/fa->page_cap]->data[(index%fa->page_cap) * fa->obj_size]);
}

void declare_array (int size, int obj_size, fixed_array *fa) {
	fa->size = size;
	fa->page_cap = MEM_PAGESIZE / obj_size;
	fa->numpages = (size-1)/(fa->page_cap) + 1;
}

