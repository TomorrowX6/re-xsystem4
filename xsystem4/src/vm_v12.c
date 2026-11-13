/* AIN v12 native runtime extensions, GPL-2.0-or-later. */
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "system4.h"
#include "system4/ain.h"
#include "system4/instructions.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "vm/v12.h"

static int stride(enum ain_data_type type)
{
	return type == AIN_IFACE ? 2 : 1;
}

// Sys43's Shuffle uses its 521-word XOR generator, not the platform C RNG.
// Fixed seeds must produce the same permutations on Windows and ARM64.
struct shuffle_random { uint32_t state[521]; int index; };

static void shuffle_random_advance(struct shuffle_random *r)
{
	for (int i = 0; i < 32; i++) r->state[i] ^= r->state[i + 489];
	for (int i = 32; i < 521; i++) r->state[i] ^= r->state[i - 32];
	r->index = 0;
}

static void shuffle_random_init(struct shuffle_random *r, uint32_t seed)
{
	uint32_t word = 0;
	for (int i = 0; i < 17; i++) {
		for (int bit = 0; bit < 32; bit++) {
			seed = seed * UINT32_C(0x5d588b65) + 1;
			word = (word >> 1) | (seed & UINT32_C(0x80000000));
		}
		r->state[i] = word;
	}
	r->state[16] = (r->state[16] << 23) ^ (r->state[0] >> 9) ^ r->state[15];
	for (int i = 17; i < 521; i++)
		r->state[i] = (r->state[i - 17] << 23) ^ (r->state[i - 16] >> 9) ^ r->state[i - 1];
	for (int i = 0; i < 4; i++) shuffle_random_advance(r);
}

static uint32_t shuffle_random_next(struct shuffle_random *r)
{
	if (r->index == 521) shuffle_random_advance(r);
	return r->state[r->index++];
}

static struct ain_type element_type(struct page *p)
{
	if (!p || p->type != ARRAY_PAGE)
		VM_ERROR("AIN v12 array operation on a non-array");
	if (p->a_type == AIN_ARRAY)
		return p->array.element_type;
	return (struct ain_type) { .data = array_type(p->a_type), .struc = p->array.struct_type };
}

int v12_array_length(struct page *p)
{
	if (!p) return 0;
	struct ain_type type = element_type(p);
	return p->nr_vars / stride(type.data);
}

struct page *v12_array_new(const struct ain_type *element, int count, bool construct)
{
	if (!element || count < 0 || count > INT_MAX / (2 * (int)sizeof(union vm_value)))
		VM_ERROR("Invalid AIN v12 array allocation: %d elements", count);
	int width = stride(element->data);
	struct page *page = alloc_page(ARRAY_PAGE, AIN_ARRAY, count * width);
	page->array.rank = 1;
	page->array.struct_type = element->struc;
	page->array.element_type = *element;
	for (int i = 0; i < count; i++) {
		if (element->data == AIN_STRUCT) {
			page->values[i * width].i = alloc_struct(element->struc);
			if (construct) init_struct(element->struc, page->values[i * width].i);
		} else {
			page->values[i * width] = variable_initval_typed(element);
		}
	}
	return page;
}

static void resize_array_mode(int slot, int count, bool construct)
{
	struct page *old = heap_get_page(slot);
	struct ain_type type = element_type(old);
	int width = stride(type.data), old_count = v12_array_length(old);
	if (count < 0 || count > INT_MAX / (2 * (int)sizeof(union vm_value)))
		VM_ERROR("Invalid AIN v12 array size: %d", count);
	struct page *page = alloc_page(ARRAY_PAGE, AIN_ARRAY, count * width);
	page->array.rank = 1;
	page->array.struct_type = type.struc;
	page->array.element_type = type;
	int keep = min(count, old_count) * width;
	memcpy(page->values, old->values, keep * sizeof(*page->values));
	for (int i = keep; i < old->nr_vars; i++)
		variable_fini(old->values[i], variable_type(old, i, NULL, NULL), true);
	free_page(old);
	heap_set_page(slot, page);
	for (int i = keep / width; i < count; i++) {
		if (type.data == AIN_STRUCT && construct) {
			int object = alloc_struct(type.struc);
			page->values[i * width].i = object;
			init_struct(type.struc, object);
		} else {
			page->values[i * width] = variable_initval_typed(&type);
		}
	}
}

static void resize_array(int slot, int count)
{
	resize_array_mode(slot, count, true);
}

static void alloc_dimensions(int slot, const int *dimensions, int remaining)
{
	resize_array(slot, 0);
	resize_array(slot, dimensions[0]);
	if (remaining < 2 || dimensions[1] == -1) return;
	if (element_type(heap_get_page(slot)).data != AIN_ARRAY)
		VM_ERROR("Too many dimensions in Array.Alloc");
	for (int i = 0; i < dimensions[0]; i++)
		alloc_dimensions(heap_get_page(slot)->values[i].i, dimensions + 1, remaining - 1);
}

static void set_element(int slot, int index, const union vm_value *value)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	int width = stride(type.data);
	if (index < 0 || index >= v12_array_length(p))
		VM_ERROR("Array index out of bounds: %d", index);
	union vm_value copy[2];
	copy[0] = vm_copy(value[0], type.data);
	if (width == 2) copy[1] = value[1];
	variable_fini(p->values[index * width], type.data, true);
	memcpy(p->values + index * width, copy, width * sizeof(*copy));
}

static void erase_element(int slot, int index)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	int width = stride(type.data), size = v12_array_length(p);
	if (index < 0 || index >= size)
		VM_ERROR("Array erase index out of bounds: %d", index);
	variable_fini(p->values[index * width], type.data, true);
	memmove(p->values + index * width, p->values + (index + 1) * width,
		(size - index - 1) * width * sizeof(*p->values));
	p->nr_vars -= width;
}

static void push_element(int slot, int index, const struct ain_type *formal)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	int pos = index * stride(type.data);
	if (index < 0 || index >= v12_array_length(p))
		VM_ERROR("Array element index out of bounds: %d", index);
	if (formal->data == AIN_WRAP) {
		if (!formal->array_type) VM_ERROR("Missing borrowed element type");
		switch (formal->array_type->data) {
		case AIN_INT: case AIN_FLOAT: case AIN_BOOL: case AIN_ENUM:
			stack_push(slot); stack_push(pos); return;
		case AIN_IFACE:
			stack_push(p->values[pos]); stack_push(p->values[pos + 1]); return;
		default:
			stack_push(p->values[pos]); return;
		}
	}
	enum ain_data_type formal_type = formal->data;
	switch (formal_type) {
	case AIN_REF_INT: case AIN_REF_FLOAT: case AIN_REF_BOOL: case AIN_REF_ENUM:
		stack_push(slot);
		stack_push(pos);
		break;
	case AIN_IFACE:
		stack_push(p->values[pos]);
		stack_push(p->values[pos + 1]);
		break;
	case AIN_STRUCT: case AIN_STRING: case AIN_ARRAY: case AIN_DELEGATE:
		stack_push(vm_copy(p->values[pos], formal_type));
		break;
	default:
		stack_push(p->values[pos]);
		break;
	}
}

static int predicate(int slot, int index, union vm_value *callback)
{
	int fn = callback[1].i;
	if (fn < 0 || fn >= ain->nr_functions || !ain->functions[fn].nr_args)
		VM_ERROR("Invalid array predicate: %d", fn);
	push_element(slot, index, &ain->functions[fn].vars[0].type);
	vm_call(fn, callback[0].i);
	return stack_pop().i;
}

static bool binary_predicate(int slot, int left, int right, union vm_value *callback)
{
	int fn = callback[1].i;
	if (fn < 0 || fn >= ain->nr_functions || ain->functions[fn].nr_args < 2)
		VM_ERROR("Invalid two-argument array predicate: %d", fn);
	struct ain_function *f = &ain->functions[fn];
	int second = 1;
	while (second < f->nr_args && f->vars[second].type.data == AIN_VOID) second++;
	if (second >= f->nr_args) VM_ERROR("Array predicate has no second argument");
	push_element(slot, left, &f->vars[0].type);
	push_element(slot, right, &f->vars[second].type);
	vm_call(fn, callback[0].i);
	return !!stack_pop().i;
}

static int compare_element(int slot, int index, union vm_value *value)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	union vm_value v = p->values[index * stride(type.data)];
	if (type.data == AIN_STRING) {
		const char *lhs = string_index_valid(v.i) ? heap_get_string(v.i)->text : "";
		const char *rhs = string_index_valid(value[0].i) ? heap_get_string(value[0].i)->text : "";
		return strcmp(lhs, rhs);
	}
	if (type.data == AIN_FLOAT)
		return (v.f > value[0].f) - (v.f < value[0].f);
	return (v.i > value[0].i) - (v.i < value[0].i);
}

static void insert_element(int slot, int index, union vm_value *value)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	int width = stride(type.data), size = v12_array_length(p);
	if (unlikely(index < 0 || index > size)) {
		static unsigned int warn_count = 0;
		if (warn_count++ < 64)
			WARNING("Array.Insert index out of bounds: %d (size=%d)", index, size);
		index = index < 0 ? 0 : size;
	}
	union vm_value copy[2] = { vm_copy(value[0], type.data), value[1] };
	resize_array_mode(slot, size + 1, false);
	p = heap_get_page(slot);
	variable_fini(p->values[size * width], type.data, true);
	memmove(p->values + (index + 1) * width, p->values + index * width,
		(size - index) * width * sizeof(*p->values));
	memcpy(p->values + index * width, copy, width * sizeof(*copy));
}

static bool equal_element(int slot, int index, union vm_value *value)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	int pos = index * stride(type.data);
	if (type.data == AIN_STRING) {
		const char *lhs = string_index_valid(p->values[pos].i) ? heap_get_string(p->values[pos].i)->text : "";
		const char *rhs = string_index_valid(value[0].i) ? heap_get_string(value[0].i)->text : "";
		return !strcmp(lhs, rhs);
	}
	if (type.data == AIN_FLOAT)
		return p->values[pos].f == value[0].f;
	return p->values[pos].i == value[0].i;
}

static void sort_array(int slot, union vm_value *callback, bool descending)
{
	struct page *p = heap_get_page(slot);
	int count = v12_array_length(p), width = stride(element_type(p).data);
	int *order = xmalloc(max(count, 1) * sizeof(int));
	int *scratch = xmalloc(max(count, 1) * sizeof(int));
	for (int i = 0; i < count; i++) order[i] = i;
	// Sort indices so callback arguments continue to refer to stable elements.
	for (int run = 1; run < count; run *= 2) {
		for (int start = 0; start < count; start += 2 * run) {
			int middle = min(start + run, count), end = min(start + 2 * run, count);
			int left = start, right = middle;
			for (int dest = start; dest < end; dest++) {
				bool use_right = left == middle;
				if (left < middle && right < end) {
					p = heap_get_page(slot);
					use_right = callback
						? binary_predicate(slot, order[right], order[left], callback)
						: (descending
							? compare_element(slot, order[right], p->values + order[left] * width) > 0
							: compare_element(slot, order[right], p->values + order[left] * width) < 0);
				}
				scratch[dest] = use_right ? order[right++] : order[left++];
			}
		}
		int *tmp = order; order = scratch; scratch = tmp;
	}
	p = heap_get_page(slot);
	if (v12_array_length(p) != count) VM_ERROR("Array size changed inside sort comparator");
	union vm_value *values = xmalloc(max(p->nr_vars, 1) * sizeof(*values));
	for (int i = 0; i < count; i++)
		memcpy(values + i * width, p->values + order[i] * width, width * sizeof(*values));
	memcpy(p->values, values, p->nr_vars * sizeof(*values));
	free(values);
	free(order);
	free(scratch);
}

static void return_element(int slot, int index)
{
	struct page *p = heap_get_page(slot);
	struct ain_type type = element_type(p);
	if (index < 0 || index >= v12_array_length(p)) {
		// At/First/Last/Min/Max return an optional reference on a missing item.
		stack_push(-1);
		if (type.data == AIN_INT || type.data == AIN_FLOAT || type.data == AIN_BOOL
				|| type.data == AIN_ENUM || type.data == AIN_IFACE)
			stack_push(0);
		return;
	}
	int pos = index * stride(type.data);
	switch (type.data) {
	case AIN_INT: case AIN_FLOAT: case AIN_BOOL: case AIN_ENUM:
		heap_ref(slot);
		stack_push(slot);
		stack_push(pos);
		break;
	case AIN_IFACE:
		heap_ref(p->values[pos].i);
		stack_push(p->values[pos]);
		stack_push(p->values[pos + 1]);
		break;
	default:
		heap_ref(p->values[pos].i);
		stack_push(p->values[pos]);
		break;
	}
}

static void cleanup_array_value_args(struct ain_hll_function *f, int parameter, union vm_value args[8][2])
{
	// Value arguments are compiler-created temporaries. Reference arguments
	// and interface pairs are borrowed for the duration of the call.
	for (int i = 1; i < f->nr_arguments; i++) {
		if (f->arguments[i].type.data != AIN_HLL_PARAM) continue;
		if (parameter == AIN_STRING || parameter == AIN_STRUCT
				|| parameter == AIN_ARRAY || parameter == AIN_DELEGATE)
			variable_fini(args[i][0], parameter, true);
	}
}

static bool array_call_invalid_slot(struct ain_hll_function *f, int parameter, int slot, union vm_value args[8][2])
{
	static unsigned int warn_count = 0;
	if (warn_count++ < 64)
		WARNING("Array.%s on invalid array slot: %d", f->name, slot);
	const char *name = f->name;
	if (!strcmp(name, "Numof") || !strcmp(name, "Any") || !strcmp(name, "IsExist")
			|| !strcmp(name, "Erase") || !strcmp(name, "EraseAll")
			|| !strcmp(name, "Copy") || !strcmp(name, "Fill")
			|| !strcmp(name, "LowerBound") || !strcmp(name, "UpperBound")) {
		stack_push(0);
	} else if (!strcmp(name, "Empty") || !strcmp(name, "All")) {
		stack_push(1);
	} else if (!strcmp(name, "Find") || !strcmp(name, "FindLast") || !strcmp(name, "BinarySearch")
			|| !strcmp(name, "Sort") || !strcmp(name, "QuickSort")
			|| !strcmp(name, "AscSort") || !strcmp(name, "DescSort")) {
		stack_push(-1);
	} else if (!strcmp(name, "ShallowCopy") || !strcmp(name, "Where")) {
		struct page *empty = alloc_page(ARRAY_PAGE, AIN_ARRAY, 0);
		empty->array.rank = 1;
		empty->array.struct_type = -1;
		empty->array.element_type = (struct ain_type){
			.data = (parameter == AIN_STRUCT || parameter == AIN_REF_STRUCT)
				? AIN_REF_STRUCT : (parameter >= 0 ? parameter : AIN_INT),
			.struc = -1,
		};
		stack_push(heap_alloc_page(empty));
	} else if (!strcmp(name, "At") || !strcmp(name, "First") || !strcmp(name, "Last")
			|| !strcmp(name, "Min") || !strcmp(name, "Max") || !strcmp(name, "EmplaceBack")) {
		stack_push(-1);
		if (parameter == AIN_INT || parameter == AIN_FLOAT || parameter == AIN_BOOL
				|| parameter == AIN_ENUM || parameter == AIN_IFACE)
			stack_push(0);
	}
	cleanup_array_value_args(f, parameter, args);
	return true;
}

static bool array_call(struct ain_hll_function *f, int parameter)
{
	union vm_value args[8][2] = {0};
	if (f->nr_arguments > 8) VM_ERROR("Too many Array arguments");
	for (int i = f->nr_arguments - 1; i >= 0; i--) {
		int type = f->arguments[i].type.data;
		int width = type == AIN_HLL_FUNC || (type == AIN_HLL_PARAM && parameter == AIN_IFACE) ? 2 : 1;
		for (int j = width - 1; j >= 0; j--) args[i][j] = stack_pop();
	}
	int slot = args[0][0].i;
	if (unlikely(!page_index_valid(slot) || (heap[slot].page && heap[slot].page->type != ARRAY_PAGE)))
		return array_call_invalid_slot(f, parameter, slot, args);
	struct page *p = heap_get_page(slot);
	int size = v12_array_length(p);
	const char *name = f->name;
	if (!strcmp(name, "Alloc")) {
		int dimensions[7];
		for (int i = 1; i < f->nr_arguments; i++) dimensions[i - 1] = args[i][0].i;
		alloc_dimensions(slot, dimensions, f->nr_arguments - 1);
	} else if (!strcmp(name, "Realloc")) {
		int new_size = max(args[1][0].i, 0);
		resize_array(slot, new_size);
		if (f->nr_arguments == 3)
			for (int i = size; i < new_size; i++) set_element(slot, i, args[2]);
	} else if (!strcmp(name, "Free")) {
		resize_array(slot, 0);
	} else if (!strcmp(name, "Numof") || !strcmp(name, "Empty") || !strcmp(name, "Any") || !strcmp(name, "All")) {
		int count = size;
		if (f->nr_arguments == 2) {
			count = 0;
			for (int i = 0; i < size; i++) count += !!predicate(slot, i, args[1]);
		}
		if (!strcmp(name, "Numof")) stack_push(count);
		else if (!strcmp(name, "Empty")) stack_push(count == 0);
		else if (!strcmp(name, "Any")) stack_push(count > 0);
		else stack_push(count == size);
	} else if (!strcmp(name, "PushBack") || !strcmp(name, "EmplaceBack")) {
		resize_array_mode(slot, size + 1, !strcmp(name, "EmplaceBack"));
		if (!strcmp(name, "PushBack")) set_element(slot, size, args[1]);
		else return_element(slot, size);
	} else if (!strcmp(name, "Insert")) {
		insert_element(slot, args[1][0].i, args[2]);
	} else if (!strcmp(name, "PopBack")) {
		if (size) resize_array(slot, size - 1);
	} else if (!strcmp(name, "Duplicate")) {
		heap_struct_assign(slot, args[1][0].i);
	} else if (!strcmp(name, "Concat")) {
		int source = args[1][0].i;
		if (unlikely(!page_index_valid(source) || !heap[source].page || heap[source].page->type != ARRAY_PAGE)) {
			WARNING("Array.Concat from invalid array slot: %d", source);
			cleanup_array_value_args(f, parameter, args);
			return true;
		}
		struct page *src = heap_get_page(source);
		struct ain_type type = element_type(p), src_type = element_type(src);
		int count = v12_array_length(src), width = stride(type.data);
		if (count == 0) {
			cleanup_array_value_args(f, parameter, args);
			return true;
		}
		if (type.data != src_type.data || type.struc != src_type.struc)
			VM_ERROR("Array.Concat element types differ");
		if (count > INT_MAX / (2 * (int)sizeof(union vm_value)) - size)
			VM_ERROR("Array.Concat result is too large");
		struct page *joined = alloc_page(ARRAY_PAGE, AIN_ARRAY, (size + count) * width);
		joined->array.rank = 1;
		joined->array.struct_type = type.struc;
		joined->array.element_type = type;
		// Copy before replacing the destination page, including for self-concat.
		// Appended value objects are copied without constructing temporary ones.
		for (int i = 0; i < count; i++) {
			src = heap_get_page(source);
			union vm_value value = src->values[i * width];
			if (width == 2) joined->values[(size + i) * width + 1] = src->values[i * width + 1];
			joined->values[(size + i) * width] = vm_copy(value, type.data);
		}
		p = heap_get_page(slot);
		memcpy(joined->values, p->values, size * width * sizeof(*p->values));
		free_page(p);
		heap_set_page(slot, joined);
	} else if (!strcmp(name, "Copy")) {
		if (f->nr_arguments != 5)
			VM_ERROR("Unimplemented Array.Copy overload with %d arguments", f->nr_arguments);
		int dst_at = args[1][0].i, source = args[2][0].i;
		int src_at = args[3][0].i, count = args[4][0].i;
		if (unlikely(!page_index_valid(source) || !heap[source].page || heap[source].page->type != ARRAY_PAGE)) {
			WARNING("Array.Copy from invalid array slot: %d", source);
			stack_push(0);
			cleanup_array_value_args(f, parameter, args);
			return true;
		}
		struct page *src = heap_get_page(source);
		struct ain_type type = element_type(p), src_type = element_type(src);
		int src_size = v12_array_length(src), width = stride(type.data);
		if (count <= 0 || dst_at < 0 || dst_at > size || count > size - dst_at
				|| src_at < 0 || src_at > src_size || count > src_size - src_at) {
			if (count != 0)
				WARNING("Array.Copy range out of bounds: dst=%d/%d src=%d/%d count=%d",
					dst_at, size, src_at, src_size, count);
			stack_push(0);
			cleanup_array_value_args(f, parameter, args);
			return true;
		}
		if (type.data != src_type.data || type.struc != src_type.struc)
			VM_ERROR("Array.Copy element types differ");
		// Snapshot values before assignment, including owned references, so
		// overlapping ranges preserve the source values in either direction.
		union vm_value *copy = xcalloc(count * width, sizeof(*copy));
		for (int i = 0; i < count; i++) {
			copy[i * width] = vm_copy(src->values[(src_at + i) * width], type.data);
			if (width == 2) copy[i * width + 1] = src->values[(src_at + i) * width + 1];
		}
		for (int i = 0; i < count; i++) {
			p = heap_get_page(slot);
			variable_fini(p->values[(dst_at + i) * width], type.data, true);
			p = heap_get_page(slot);
			if (dst_at + i >= v12_array_length(p))
				VM_ERROR("Array.Copy destination changed during assignment");
			memcpy(p->values + (dst_at + i) * width, copy + i * width, width * sizeof(*copy));
		}
		free(copy);
		stack_push(count);
	} else if (!strcmp(name, "ShallowCopy")) {
		struct page *copy = alloc_page(ARRAY_PAGE, AIN_ARRAY, p->nr_vars);
		copy->array = p->array;
		if (copy->array.element_type.data == AIN_STRUCT
				|| parameter == AIN_STRUCT || parameter == AIN_REF_STRUCT)
			copy->array.element_type.data = AIN_REF_STRUCT;
		else if (parameter >= 0)
			copy->array.element_type.data = parameter;
		memcpy(copy->values, p->values, p->nr_vars * sizeof(*p->values));
		for (int i = 0; i < copy->nr_vars; i++) {
			switch (variable_type(copy, i, NULL, NULL)) {
			case AIN_STRING: case AIN_STRUCT: case AIN_ARRAY: case AIN_DELEGATE:
			case AIN_REF_TYPE: case AIN_IFACE:
				heap_ref(copy->values[i].i);
				break;
			default: break;
			}
		}
		stack_push(heap_alloc_page(copy));
	} else if (!strcmp(name, "Where")) {
		struct ain_type type = element_type(p);
		int width = stride(type.data), count = 0;
		struct page *result = alloc_page(ARRAY_PAGE, AIN_ARRAY, size * width);
		result->array = p->array;
		for (int i = 0; i < size; i++) {
			if (!predicate(slot, i, args[1])) continue;
			// A callback may allocate heap slots or replace the source page.
			p = heap_get_page(slot);
			result->values[count * width] = vm_copy(p->values[i * width], type.data);
			if (width == 2) result->values[count * width + 1] = p->values[i * width + 1];
			count++;
		}
		result->nr_vars = count * width;
		stack_push(heap_alloc_page(result));
	} else if (!strcmp(name, "At") || !strcmp(name, "First") || !strcmp(name, "Last")) {
		int index = !strcmp(name, "At") ? args[1][0].i : !strcmp(name, "Last") ? size - 1 : 0;
		if (f->nr_arguments == 2 && f->arguments[1].type.data == AIN_HLL_FUNC) {
			int step = !strcmp(name, "Last") ? -1 : 1;
			for (; index >= 0 && index < size; index += step)
				if (predicate(slot, index, args[1])) break;
		}
		return_element(slot, index);
	} else if (!strcmp(name, "Min") || !strcmp(name, "Max")) {
		struct ain_type type = element_type(p);
		if (!size) {
			return_element(slot, -1);
		} else {
			int best = 0;
			bool maximum = !strcmp(name, "Max");
			for (int i = 1; i < size; i++) {
				p = heap_get_page(slot);
				bool less = f->nr_arguments == 2
					? binary_predicate(slot, i, best, args[1])
					: compare_element(slot, i, p->values + best * stride(type.data)) < 0;
				// The original chooses the first minimum and the last maximum.
				if (maximum ? !less : less) best = i;
			}
			return_element(slot, best);
		}
	} else if (!strcmp(name, "Find") || !strcmp(name, "FindLast") || !strcmp(name, "IsExist")) {
		int arg = f->nr_arguments - 1, found = -1;
		int begin = f->nr_arguments == 4 ? args[1][0].i : 0;
		int end = f->nr_arguments == 4 ? args[2][0].i : size;
		bool backwards = !strcmp(name, "FindLast");
		for (int i = backwards ? end - 1 : begin; i >= begin && i < end; i += backwards ? -1 : 1) {
			bool match = f->arguments[arg].type.data == AIN_HLL_FUNC
				? predicate(slot, i, args[arg]) : equal_element(slot, i, args[arg]);
			if (match) { found = i; break; }
		}
		if (!strcmp(name, "IsExist")) stack_push(found >= 0); else stack_push(found);
	} else if (!strcmp(name, "BinarySearch") || !strcmp(name, "LowerBound") || !strcmp(name, "UpperBound")) {
		int first = 0, last = size;
		bool callback = f->arguments[1].type.data == AIN_HLL_FUNC;
		bool upper = !strcmp(name, "UpperBound");
		while (first < last) {
			int mid = first + (last - first) / 2;
			int cmp = callback ? predicate(slot, mid, args[1]) : compare_element(slot, mid, args[1]);
			if (cmp < 0 || (upper && cmp == 0)) first = mid + 1;
			else last = mid;
		}
		if (!strcmp(name, "BinarySearch")) {
			if (first == size || (callback ? predicate(slot, first, args[1]) : compare_element(slot, first, args[1])))
				first = -1;
		}
		stack_push(first);
	} else if (!strcmp(name, "Erase") || !strcmp(name, "EraseAll")) {
		bool erased = false;
		if (f->arguments[1].type.data == AIN_INT) {
			int index = args[1][0].i;
			if (index >= 0 && index < size) { erase_element(slot, index); erased = true; }
		} else if (f->arguments[1].type.data == AIN_HLL_FUNC) {
			for (int i = 0; i < size; ) {
				if (predicate(slot, i, args[1])) {
					erase_element(slot, i); size--; erased = true;
					if (!strcmp(name, "Erase")) break;
				} else i++;
			}
		} else if (f->arguments[1].type.data == AIN_REF_ARRAY) {
			int src_slot = args[1][0].i;
			if (page_index_valid(src_slot) && heap[src_slot].page && heap[src_slot].page->type == ARRAY_PAGE) {
				struct page *src = heap_get_page(src_slot);
				struct ain_type type = element_type(p), src_type = element_type(src);
				int src_size = v12_array_length(src), width = stride(type.data);
				if (src_size > 0 && (type.data != src_type.data || type.struc != src_type.struc))
					VM_ERROR("Array.Erase element types differ");
				for (int j = 0; j < src_size; j++) {
					src = heap_get_page(src_slot);
					for (int i = 0; i < size; i++) {
						if (equal_element(slot, i, src->values + j * width)) {
							erase_element(slot, i);
							size--;
							erased = true;
							break;
						}
					}
				}
			}
		} else VM_ERROR("Array.Erase overload is not implemented");
		stack_push(erased);
	} else if (!strcmp(name, "Fill")) {
		int first = f->nr_arguments == 4 ? max(args[1][0].i, 0) : 0;
		int count = f->nr_arguments == 4 ? max(args[2][0].i, 0) : size;
		if (first > size) first = size;
		if (count > size - first) count = size - first;
		for (int i = first; i < first + count; i++) set_element(slot, i, args[f->nr_arguments - 1]);
		stack_push(count);
	} else if (!strcmp(name, "Reverse")) {
		int width = stride(element_type(p).data);
		for (int i = 0; i < size / 2; i++) {
			for (int j = 0; j < width; j++) {
				union vm_value tmp = p->values[i * width + j];
				p->values[i * width + j] = p->values[(size - 1 - i) * width + j];
				p->values[(size - 1 - i) * width + j] = tmp;
			}
		}
	} else if (!strcmp(name, "Shuffle")) {
		if (size > 1) {
			struct shuffle_random random;
			int seed = args[1][0].i, width = stride(element_type(p).data);
			shuffle_random_init(&random, seed < 0 ? SDL_GetTicks() : (uint32_t)seed);
			for (int i = 1; i < size; i++) {
				int at = shuffle_random_next(&random) % (uint32_t)(i + 1);
				// Swap complete values, preserving ownership and interface pairs.
				for (int j = 0; j < width; j++) {
					union vm_value tmp = p->values[i * width + j];
					p->values[i * width + j] = p->values[at * width + j];
					p->values[at * width + j] = tmp;
				}
			}
		}
	} else if (!strcmp(name, "Sort") || !strcmp(name, "QuickSort")
			|| !strcmp(name, "AscSort") || !strcmp(name, "DescSort")) {
		sort_array(slot, f->nr_arguments == 2 ? args[1] : NULL, !strcmp(name, "DescSort"));
		heap_ref(slot);
		stack_push(slot);
	} else if (!strcmp(name, "Unique")) {
		// Keep the first occurrence, including when equal values are separated.
		int width = stride(element_type(p).data);
		for (int i = 0; i < size; i++) {
			for (int j = i + 1; j < size;) {
				p = heap_get_page(slot);
				bool equal = f->nr_arguments == 2 ? binary_predicate(slot, i, j, args[1])
					: equal_element(slot, j, p->values + i * width);
				if (equal) {
					erase_element(slot, j);
					size--;
				} else j++;
			}
		}
	} else {
		VM_ERROR("Unimplemented AIN v12 Array.%s overload (%d arguments)", name, f->nr_arguments);
	}
	cleanup_array_value_args(f, parameter, args);
	return true;
}

static int encoded_char_size(const char *text)
{
	return SJIS_2BYTE((unsigned char)*text) && text[1] ? 2 : 1;
}

static int string_find_last(const struct string *s, const struct string *match)
{
	int found = -1, index = 0;
	// Indices count characters, and a trail byte must never start a match.
	// The original v12 runtime excludes the terminator even for an empty match.
	for (int offset = 0; offset < s->size; offset += encoded_char_size(s->text + offset), index++) {
		if (match->size <= s->size - offset && !memcmp(s->text + offset, match->text, match->size))
			found = index;
	}
	return found;
}

static struct page *split_string(struct string *s, struct string *separators, bool keep)
{
	// Separators is a set of characters. Empty fields are omitted, while
	// ContainSeparator returns each delimiter as a separate array element.
	struct page *list = alloc_page(ARRAY_PAGE, AIN_ARRAY, s->size);
	list->array.rank = 1;
	list->array.struct_type = -1;
	list->array.element_type = (struct ain_type){.data = AIN_STRING, .struc = -1};
	int count = 0;
	const char *begin = s->text, *at = begin;
	while (*at) {
		int width = encoded_char_size(at);
		bool separator = false;
		for (const char *sep = separators->text; *sep; sep += encoded_char_size(sep)) {
			if (encoded_char_size(sep) == width && !memcmp(at, sep, width)) {
				separator = true;
				break;
			}
		}
		if (separator) {
			if (at > begin) list->values[count++].i = heap_alloc_string(make_string(begin, at - begin));
			if (keep) list->values[count++].i = heap_alloc_string(make_string(at, width));
			begin = at + width;
		}
		at += width;
	}
	if (at > begin) list->values[count++].i = heap_alloc_string(make_string(begin, at - begin));
	list->nr_vars = count;
	return list;
}

static struct string *replace_string(struct string *s, struct string *match, struct string *replacement)
{
	// An empty pattern must not leave the replacement loop without progress.
	if (!match->size) return string_dup(s);
	struct string *out = string_ref(&EMPTY_STRING);
	int begin = 0, at = 0;
	while (at < s->size) {
		if (match->size <= s->size - at && !memcmp(s->text + at, match->text, match->size)) {
			string_append_cstr(&out, s->text + begin, at - begin);
			string_append(&out, replacement);
			at += match->size;
			begin = at;
		} else {
			at += encoded_char_size(s->text + at);
		}
	}
	string_append_cstr(&out, s->text + begin, s->size - begin);
	return out;
}

struct regex_token {
	bool is_dot;
	const char *bytes;
	int width;
	int min_rep;
	int max_rep;
	bool greedy;
};

static int regex_token_matches(const struct regex_token *tok, const struct string *s, int off)
{
	if (off >= s->size) return 0;
	int w = encoded_char_size(s->text + off);
	if (tok->is_dot) return (w == 1 && s->text[off] == '\n') ? 0 : w;
	return (w == tok->width && !memcmp(s->text + off, tok->bytes, w)) ? w : 0;
}

static int regex_match_at(const struct regex_token *tokens, int nr_tokens, int idx, const struct string *s, int off);

static int regex_match_greedy(const struct regex_token *tokens, int nr_tokens, int idx, const struct string *s, int off, int rem)
{
	if (rem > 0) {
		int w = regex_token_matches(&tokens[idx], s, off);
		if (w > 0) {
			int end = regex_match_greedy(tokens, nr_tokens, idx, s, off + w, rem - 1);
			if (end >= 0) return end;
		}
	}
	return regex_match_at(tokens, nr_tokens, idx + 1, s, off);
}

static int regex_match_at(const struct regex_token *tokens, int nr_tokens, int idx, const struct string *s, int off)
{
	if (idx >= nr_tokens) return off;
	const struct regex_token *tok = &tokens[idx];
	int cur = off;
	for (int i = 0; i < tok->min_rep; i++) {
		int w = regex_token_matches(tok, s, cur);
		if (!w) return -1;
		cur += w;
	}
	int extra = tok->max_rep - tok->min_rep;
	if (!tok->greedy) {
		for (int i = 0; ; i++) {
			int end = regex_match_at(tokens, nr_tokens, idx + 1, s, cur);
			if (end >= 0) return end;
			if (i >= extra) break;
			int w = regex_token_matches(tok, s, cur);
			if (!w) break;
			cur += w;
		}
		return -1;
	}
	return regex_match_greedy(tokens, nr_tokens, idx, s, cur, extra);
}

static struct string *replace_regex_string(struct string *s, struct string *pattern, struct string *replacement)
{
	if (!pattern->size) return string_dup(s);
	struct regex_token *tokens = xcalloc(pattern->size, sizeof(*tokens));
	int nr_tokens = 0;
	for (int off = 0; off < pattern->size; ) {
		struct regex_token tok = { .min_rep = 1, .max_rep = 1, .greedy = true };
		if (pattern->text[off] == '\\' && off + 1 < pattern->size) {
			off++;
			tok.bytes = pattern->text + off;
			tok.width = encoded_char_size(pattern->text + off);
			off += tok.width;
		} else if (pattern->text[off] == '.') {
			tok.is_dot = true;
			off++;
		} else {
			tok.bytes = pattern->text + off;
			tok.width = encoded_char_size(pattern->text + off);
			off += tok.width;
		}
		if (off < pattern->size && (pattern->text[off] == '*' || pattern->text[off] == '+' || pattern->text[off] == '?')) {
			char q = pattern->text[off++];
			tok.min_rep = (q == '+') ? 1 : 0;
			tok.max_rep = (q == '?') ? 1 : s->size + 1;
			if (off < pattern->size && pattern->text[off] == '?') {
				tok.greedy = false;
				off++;
			}
		}
		tokens[nr_tokens++] = tok;
	}
	struct string *out = string_ref(&EMPTY_STRING);
	int begin = 0, at = 0;
	while (at < s->size) {
		int end = regex_match_at(tokens, nr_tokens, 0, s, at);
		if (end > at) {
			string_append_cstr(&out, s->text + begin, at - begin);
			string_append(&out, replacement);
			at = end;
			begin = at;
		} else {
			at += encoded_char_size(s->text + at);
		}
	}
	string_append_cstr(&out, s->text + begin, s->size - begin);
	free(tokens);
	return out;
}

static bool string_call(struct ain_hll_function *f)
{
	union vm_value args[5];
	if (f->nr_arguments > 5) VM_ERROR("Too many String arguments");
	for (int i = f->nr_arguments - 1; i >= 0; i--) args[i] = stack_pop();
	int slot = args[0].i;
	bool valid_slot = string_index_valid(slot);
	if (unlikely(!valid_slot)) {
		static unsigned int warn_count = 0;
		if (warn_count++ < 64)
			WARNING("String.%s on invalid string slot: %d", f->name, slot);
	}
	struct string *s = valid_slot ? heap_get_string(slot) : &EMPTY_STRING, *result = NULL;
	const char *name = f->name;
	if (!strcmp(name, "Int")) stack_push(string_to_integer(s));
	else if (!strcmp(name, "Float")) stack_push((float)strtod(s->text, NULL));
	else if (!strcmp(name, "LengthByte")) stack_push(s->size);
	else if (!strcmp(name, "Length")) {
		stack_push(sjis_count_char(s->text));
	} else if (!strcmp(name, "Empty")) stack_push(s->size == 0);
	else if (!strcmp(name, "GetPart")) result = string_copy(s, args[1].i, f->nr_arguments == 3 ? args[2].i : s->size);
	else if (!strcmp(name, "Replace")) result = replace_string(s, string_index_valid(args[1].i) ? heap_get_string(args[1].i) : &EMPTY_STRING, string_index_valid(args[2].i) ? heap_get_string(args[2].i) : &EMPTY_STRING);
	else if (!strcmp(name, "ReplaceRegex")) result = replace_regex_string(s, string_index_valid(args[1].i) ? heap_get_string(args[1].i) : &EMPTY_STRING, string_index_valid(args[2].i) ? heap_get_string(args[2].i) : &EMPTY_STRING);
	else if (!strcmp(name, "Find") || !strcmp(name, "FindLast") || !strcmp(name, "Contains")) {
		struct string *match = string_index_valid(args[1].i) ? heap_get_string(args[1].i) : &EMPTY_STRING;
		int index = !strcmp(name, "FindLast") ? string_find_last(s, match) : string_find(s, match);
		if (!strcmp(name, "Contains")) stack_push(index >= 0); else stack_push(index);
	} else if (!strcmp(name, "StartsWith") || !strcmp(name, "EndsWith")) {
		struct string *match = string_index_valid(args[1].i) ? heap_get_string(args[1].i) : &EMPTY_STRING;
		int offset = !strcmp(name, "EndsWith") ? s->size - match->size : 0;
		stack_push(offset >= 0 && match->size <= s->size && !memcmp(s->text + offset, match->text, match->size));
	} else if (!strcmp(name, "PushBack")) {
		if (valid_slot) string_push_back(&heap[slot].s, args[1].i);
	} else if (!strcmp(name, "Split")) {
		stack_push(heap_alloc_page(split_string(s, string_index_valid(args[1].i) ? heap_get_string(args[1].i) : &EMPTY_STRING, args[2].i)));
	}
	else if (!strcmp(name, "PopBack")) {
		if (valid_slot) string_pop_back(&heap[slot].s);
	} else if (!strcmp(name, "Erase")) {
		if (valid_slot) {
			for (int i = 0; i < args[2].i; i++) string_erase(&heap[slot].s, args[1].i);
		}
	} else if (!strcmp(name, "ToUpper") || !strcmp(name, "ToLower")) {
		result = string_dup(s);
		for (int i = 0; i < result->size; i++) {
			unsigned char c = result->text[i];
			if (c < 0x80) result->text[i] = !strcmp(name, "ToUpper") ? toupper(c) : tolower(c);
			else if (SJIS_2BYTE(c) && i + 1 < result->size) i++;
		}
	} else VM_ERROR("Unimplemented AIN v12 String.%s overload (%d arguments)", name, f->nr_arguments);
	for (int i = 1; i < f->nr_arguments; i++)
		if (f->arguments[i].type.data == AIN_STRING) heap_unref(args[i].i);
	if (result) stack_push(heap_alloc_string(result));
	return true;
}

static bool delegate_call_hll(struct ain_hll_function *f)
{
	int fn = -1, object = -1;
	if (f->nr_arguments == 2) { fn = stack_pop().i; object = stack_pop().i; }
	int slot = stack_pop().i;
	if (unlikely(!page_index_valid(slot))) {
		static unsigned int warn_count = 0;
		if (warn_count++ < 64)
			WARNING("Delegate.%s on invalid delegate slot: %d", f->name, slot);
		if (!strcmp(f->name, "Numof") || !strcmp(f->name, "Exist"))
			stack_push(0);
		else if (!strcmp(f->name, "Empty"))
			stack_push(1);
		return true;
	}
	struct page *page = heap_get_page(slot);
	if (!strcmp(f->name, "Set")) {
		page = delegate_clear(page);
		heap_set_page(slot, delegate_append(page, object, fn));
	} else if (!strcmp(f->name, "Add")) heap_set_page(slot, delegate_append(page, object, fn));
	else if (!strcmp(f->name, "Numof")) stack_push(delegate_numof(page));
	else if (!strcmp(f->name, "Empty")) stack_push(delegate_numof(page) == 0);
	else if (!strcmp(f->name, "Exist")) stack_push(delegate_contains(page, object, fn));
	else if (!strcmp(f->name, "Erase")) delegate_erase(page, object, fn);
	else if (!strcmp(f->name, "Clear")) heap_set_page(slot, delegate_clear(page));
	else VM_ERROR("Unimplemented AIN v12 Delegate.%s", f->name);
	return true;
}

bool v12_builtin_library(const char *name)
{
	return !strcmp(name, "system") || !strcmp(name, "Array") || !strcmp(name, "String")
		|| !strcmp(name, "Delegate") || !strcmp(name, "Int") || !strcmp(name, "Float");
}

bool v12_hll_call(int library, int function, int type_parameter)
{
	if (library < 0 || library >= ain->nr_libraries || function < 0
			|| function >= ain->libraries[library].nr_functions)
		VM_ERROR("Invalid AIN v12 HLL call: %d.%d", library, function);
	struct ain_library *lib = &ain->libraries[library];
	struct ain_hll_function *f = &lib->functions[function];
	if (!strcmp(lib->name, "system")) {
		if (!strcmp(f->name, "ExistSaveFile")) {
			vm_system_call(SYS_EXISTS_SAVE_FILE);
			return true;
		}
		for (int i = 0; i < NR_SYSCALLS; i++) {
			if (syscalls[i].name && !strcmp(syscalls[i].name + 7, f->name)) {
				vm_system_call(i);
				return true;
			}
		}
		VM_ERROR("Unimplemented AIN v12 system.%s", f->name);
	}
	if (!strcmp(lib->name, "Array")) return array_call(f, type_parameter);
	if (!strcmp(lib->name, "String")) return string_call(f);
	if (!strcmp(lib->name, "Delegate")) return delegate_call_hll(f);
	if (!strcmp(lib->name, "Int") && !strcmp(f->name, "String")) {
		if (f->nr_arguments != 1) VM_ERROR("Unexpected Int.String signature");
		int index = stack_pop().i, page = stack_pop().i;
		int32_t val = (page_index_valid(page) && heap[page].page && index >= 0 && index < heap[page].page->nr_vars)
			? heap[page].page->values[index].i : 0;
		stack_push(heap_alloc_string(integer_to_string(val)));
		return true;
	}
	if (!strcmp(lib->name, "Float") && !strcmp(f->name, "String")) {
		int precision = f->nr_arguments == 2 ? stack_pop().i : 6;
		int index = stack_pop().i, page = stack_pop().i;
		float val = (page_index_valid(page) && heap[page].page && index >= 0 && index < heap[page].page->nr_vars)
			? heap[page].page->values[index].f : 0.0f;
		stack_push(heap_alloc_string(float_to_string(val, precision)));
		return true;
	}
	return false;
}
