import { useMemo, useState } from 'react';

export interface GroupOption {
    value: string;
}

export const useFilterableGroupOptions = (groups: string[]) => {
    const [searchText, setSearchText] = useState('');
    const [showAllOnOpen, setShowAllOnOpen] = useState(false);

    const options = useMemo(
        () =>
            groups
                .map((value) => value.trim())
                .filter(Boolean)
                .sort((a, b) => a.localeCompare(b, 'zh-Hans-CN'))
                .map((value) => ({ value })),
        [groups]
    );

    const filteredOptions = useMemo(() => {
        if (showAllOnOpen || !searchText.trim()) {
            return options;
        }

        const keyword = searchText.trim().toLowerCase();
        return options.filter((option) => option.value.toLowerCase().includes(keyword));
    }, [options, searchText, showAllOnOpen]);

    return {
        options: filteredOptions,
        onDropdownVisibleChange: (open: boolean) => {
            setShowAllOnOpen(open);
            if (!open) {
                setSearchText('');
            }
        },
        onSearch: (value: string) => {
            setSearchText(value);
            setShowAllOnOpen(false);
        },
    };
};
