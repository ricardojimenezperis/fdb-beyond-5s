bool PagePool::ageException(uint32_t id) const {
    return ageEntry(id).previous == id;
}
bool PagePool::exceptionLess(uint32_t a, uint32_t b) const {
    const Version av = ageEntry(a).maxCV, bv = ageEntry(b).maxCV;
    return av < bv || (av == bv && a < b);
}
void PagePool::exceptionSwap(size_t a, size_t b) {
    std::swap(exceptionAges_[a], exceptionAges_[b]);
    ageEntry(exceptionAges_[a]).next = static_cast<uint32_t>(a);
    ageEntry(exceptionAges_[b]).next = static_cast<uint32_t>(b);
    ++exceptionStats_.swaps;
}
void PagePool::exceptionRepair(size_t pos) {
    while (pos && exceptionLess(exceptionAges_[pos], exceptionAges_[(pos - 1) / 2])) {
        const size_t parent = (pos - 1) / 2;
        exceptionSwap(pos, parent);
        pos = parent;
    }
    for (;;) {
        const size_t left = pos * 2 + 1;
        if (left >= exceptionAges_.size()) break;
        const size_t right = left + 1;
        const size_t best = right < exceptionAges_.size() &&
            exceptionLess(exceptionAges_[right], exceptionAges_[left]) ? right : left;
        if (!exceptionLess(exceptionAges_[best], exceptionAges_[pos])) break;
        exceptionSwap(pos, best);
        pos = best;
    }
}
void PagePool::reserveExceptionSlot() {
    if (exceptionAges_.size() == exceptionAges_.capacity()) {
        if (exceptionAges_.size() >= NoAge - 1) throw std::length_error("exception age capacity");
        const size_t capacity = std::min<size_t>(NoAge - 1,
            std::max<size_t>(8, exceptionAges_.capacity() * 2));
        exceptionAges_.reserve(capacity);
    }
}
void PagePool::insertException(uint32_t id) {
    ASSERT(exceptionAges_.size() < exceptionAges_.capacity());
    auto& entry = ageEntry(id);
    entry.previous = id;
    entry.next = static_cast<uint32_t>(exceptionAges_.size());
    exceptionAges_.push_back(id);
    exceptionRepair(exceptionAges_.size() - 1);
    ++exceptionStats_.insertions;
    exceptionStats_.peak = std::max<uint64_t>(exceptionStats_.peak, exceptionAges_.size());
}
void PagePool::eraseException(uint32_t id) {
    const size_t pos = ageEntry(id).next;
    ASSERT(pos < exceptionAges_.size() && exceptionAges_[pos] == id);
    const uint32_t last = exceptionAges_.back();
    exceptionAges_.pop_back();
    if (pos < exceptionAges_.size()) {
        exceptionAges_[pos] = last;
        ageEntry(last).next = static_cast<uint32_t>(pos);
        exceptionRepair(pos);
    }
    ageEntry(id).previous = ageEntry(id).next = NoAge;
    ++exceptionStats_.removals;
}
void PagePool::appendMainAge(uint32_t id) {
    auto& entry = ageEntry(id);
    ASSERT(newestAge_ == NoAge || ageEntry(newestAge_).maxCV <= entry.maxCV);
    entry.previous = newestAge_;
    entry.next = NoAge;
    if (newestAge_ != NoAge) ageEntry(newestAge_).next = id;
    else oldestAge_ = id;
    newestAge_ = id;
}
ReclaimLink* PagePool::oldestAge() const {
    if (exceptionAges_.empty()) return ageObject(oldestAge_);
    const uint32_t top = exceptionAges_.front();
    if (oldestAge_ == NoAge || ageEntry(top).maxCV < ageEntry(oldestAge_).maxCV)
        return ageObject(top);
    return ageObject(oldestAge_);
}
ReclaimLink* PagePool::adjacentAge(const ReclaimLink* link, bool forward) const {
    if (!link->ageLinked) return nullptr;
    size_t ordinal = 0, currentOrdinal = 0;
    bool found = false;
    auto locate = [&](uint32_t id) {
        if (id == link->ageIndex) { currentOrdinal = ordinal; found = true; }
        ++ordinal;
    };
    for (uint32_t id = oldestAge_; id != NoAge; id = ageEntry(id).next) locate(id);
    for (uint32_t id : exceptionAges_) locate(id);
    ASSERT(found);
    const Version currentCV = ageEntry(link->ageIndex).maxCV;
    uint32_t best = NoAge;
    Version bestCV = 0;
    size_t bestOrdinal = 0;
    ordinal = 0;
    auto visit = [&](uint32_t id) {
        const Version cv = ageEntry(id).maxCV;
        const bool after = cv > currentCV || (cv == currentCV && ordinal > currentOrdinal);
        const bool before = cv < currentCV || (cv == currentCV && ordinal < currentOrdinal);
        const bool eligible = forward ? after : before;
        const bool better = best == NoAge || (forward
            ? (cv < bestCV || (cv == bestCV && ordinal < bestOrdinal))
            : (cv > bestCV || (cv == bestCV && ordinal > bestOrdinal)));
        if (eligible && better) { best = id; bestCV = cv; bestOrdinal = ordinal; }
        ++ordinal;
    };
    for (uint32_t id = oldestAge_; id != NoAge; id = ageEntry(id).next) visit(id);
    for (uint32_t id : exceptionAges_) visit(id);
    return ageObject(best);
}
ReclaimLink* PagePool::nextAge(const ReclaimLink* link) const { return adjacentAge(link, true); }
ReclaimLink* PagePool::previousAge(const ReclaimLink* link) const { return adjacentAge(link, false); }
Version PagePool::oldestAgeCV() const {
    const auto* link = oldestAge();
    ASSERT(link);
    return ageEntry(link->ageIndex).maxCV;
}
size_t PagePool::ageMetadataBytes() const {
    return nodeAges_.capacity() * sizeof(AgeEntry)
        + (pageAgeBlocks_.capacity() + nodeBlockPages_.capacity() + freeAgeBlocks_.capacity()) * sizeof(uint32_t)
        + exceptionAgeMetadataBytes();
}
void PagePool::unlinkAge(ReclaimLink* link) {
    if (!link->ageLinked) return;
    auto& entry = ageEntry(link->ageIndex);
    if (ageException(link->ageIndex)) eraseException(link->ageIndex);
    else {
        if (entry.previous != NoAge) ageEntry(entry.previous).next = entry.next;
        else oldestAge_ = entry.next;
        if (entry.next != NoAge) ageEntry(entry.next).previous = entry.previous;
        else newestAge_ = entry.previous;
        entry.previous = entry.next = NoAge;
    }
    link->ageLinked = false;
}
void PagePool::raiseAge(ReclaimLink* link, Version cv, PageUse use) {
    if (link->ageLinked && cv <= link->maxCV) return;
    ASSERT(cv >= link->maxCV);
    ensureAgeIndex(link, use);
    const bool exceptional = newestAge_ != NoAge && cv < ageEntry(newestAge_).maxCV;
    if (exceptional && link->ageLinked && ageException(link->ageIndex)) {
        link->maxCV = ageEntry(link->ageIndex).maxCV = cv;
        link->ageUse = use;
        exceptionRepair(ageEntry(link->ageIndex).next);
        ++exceptionStats_.updates;
        return;
    }
    if (exceptional) reserveExceptionSlot();
    unlinkAge(link);
    link->maxCV = cv; link->ageUse = use;
    ageEntry(link->ageIndex).maxCV = cv;
    if (exceptional) insertException(link->ageIndex);
    else appendMainAge(link->ageIndex);
    link->ageLinked = true;
}
void PagePool::lowerNodeAgeAfterSplit(ReclaimLink* link, Version cv) {
    ASSERT(link->ageLinked && link->ageUse == PageUse::Index);
    auto& entry = ageEntry(link->ageIndex);
    ASSERT(entry.maxCV == link->maxCV && cv <= link->maxCV);
    if (cv == link->maxCV) return;
    if (ageException(link->ageIndex)) {
        link->maxCV = entry.maxCV = cv;
        exceptionRepair(entry.next);
        ++exceptionStats_.updates;
        return;
    }
    if (entry.previous == NoAge || ageEntry(entry.previous).maxCV <= cv) {
        link->maxCV = entry.maxCV = cv;
        return;
    }
    reserveExceptionSlot();
    unlinkAge(link);
    link->maxCV = entry.maxCV = cv;
    insertException(link->ageIndex);
    link->ageLinked = true;
}
void PagePool::inheritAge(ReclaimLink* link, const ReclaimLink* source, PageUse use) {
    ASSERT(source != link && source->ageLinked);
    ensureAgeIndex(link, use);
    const bool exceptional = ageException(source->ageIndex);
    if (exceptional) reserveExceptionSlot();
    unlinkAge(link);
    auto& entry = ageEntry(link->ageIndex);
    const Version cv = ageEntry(source->ageIndex).maxCV;
    link->maxCV = entry.maxCV = cv; link->ageUse = use;
    if (exceptional) insertException(link->ageIndex);
    else {
        auto& previous = ageEntry(source->ageIndex);
        entry.previous = source->ageIndex; entry.next = previous.next;
        if (entry.next != NoAge) ageEntry(entry.next).previous = link->ageIndex;
        else newestAge_ = link->ageIndex;
        previous.next = link->ageIndex;
    }
    link->ageLinked = true;
}
